// Test-server command channel. Every 0.5 s the mission reads $profile:dayzvr/cmd.txt,
// executes each line against the first connected player, deletes the file and appends
// results to $profile:dayzvr/cmd.log (read back by scripts/dayz-cmd.sh).
//
//   give <class> [count]       item(s) into the inventory (ground if full)
//   hands <class> [magclass]   weapon into hands with a full attached magazine
//                              (default magazine from the weapon config), or any item;
//                              a held item is dropped first and the creation runs
//                              on the next tick (two "->" lines in cmd.log)
//   spawn <class> [dx dy dz]   object in front of the player (vehicles get wheels,
//                              battery, spark plug, radiator, fuel, water, oil)
//   tp <x> <z> | tp <x> <y> <z> | tp <preset>   teleport (presets: nwaf, cherno,
//                              elektro, berezino, svetlo, tisy, zeleno)
//   heal                       full health/blood/energy/water, no bleeding
//   time <hour> [minute]       set the server clock
//   weather clear|rain|fog     set weather instantly
//   kill                       kill the player (respawn tests)
class DayZVRServerCmd
{
	static const string DIR = "$profile:dayzvr/";
	static const float INTERVAL = 0.5;
	protected float m_Accumulated;
	// Commands that need the next tick (an item was just dropped from the hands and
	// the slot only frees after the frame): executed before reading a new cmd.txt.
	protected ref array<string> m_Deferred = new array<string>();

	void Tick(float timeslice)
	{
		m_Accumulated += timeslice;
		if (m_Accumulated < INTERVAL)
			return;
		m_Accumulated = 0;
		array<string> lines = new array<string>();
		foreach (string deferred : m_Deferred)
			lines.Insert(deferred);
		m_Deferred.Clear();
		if (!FileExist(DIR + "cmd.txt"))
		{
			foreach (string retry : lines)
				Log(retry + " -> " + Execute(retry));
			return;
		}
		FileHandle file = OpenFile(DIR + "cmd.txt", FileMode.READ);
		if (!file)
			return;
		string line;
		while (FGets(file, line) >= 0)
		{
			line = line.Trim();
			if (line != "")
				lines.Insert(line);
		}
		CloseFile(file);
		DeleteFile(DIR + "cmd.txt");
		foreach (string command : lines)
			Log(command + " -> " + Execute(command));
	}

	protected void Log(string text)
	{
		Print("[DayZVR] " + text);
		FileHandle file = OpenFile(DIR + "cmd.log", FileMode.APPEND);
		if (!file)
			return;
		FPrintln(file, text);
		CloseFile(file);
	}

	protected PlayerBase FirstPlayer()
	{
		array<Man> players = new array<Man>();
		GetGame().GetPlayers(players);
		if (players.Count() == 0)
			return null;
		return PlayerBase.Cast(players[0]);
	}

	protected string Execute(string command)
	{
		array<string> words = new array<string>();
		command.Split(" ", words);
		if (words.Count() == 0)
			return "empty";
		string verb = words[0];
		verb.ToLower();
		PlayerBase player = FirstPlayer();
		if (!player)
			return "no player connected";
		if (verb == "give")
			return Give(player, words);
		if (verb == "hands")
			return Hands(player, words);
		if (verb == "spawn")
			return Spawn(player, words);
		if (verb == "tp")
			return Teleport(player, words);
		if (verb == "heal")
			return Heal(player);
		if (verb == "time")
			return SetTime(words);
		if (verb == "weather")
			return SetWeather(words);
		if (verb == "kill")
		{
			player.SetHealth("", "", 0);
			return "killed";
		}
		return "unknown command";
	}

	protected string Give(PlayerBase player, array<string> words)
	{
		if (words.Count() < 2)
			return "usage: give <class> [count]";
		int count = 1;
		if (words.Count() > 2)
			count = words[2].ToInt();
		int created = 0;
		for (int i = 0; i < count; i++)
		{
			EntityAI item = player.GetInventory().CreateInInventory(words[1]);
			if (!item)
				item = EntityAI.Cast(GetGame().CreateObjectEx(words[1], player.GetPosition() + player.GetDirection(), ECE_PLACE_ON_SURFACE));
			if (item)
				created++;
		}
		return "created " + created.ToString() + "x " + words[1];
	}

	protected string Hands(PlayerBase player, array<string> words)
	{
		if (words.Count() < 2)
			return "usage: hands <class> [magclass]";
		EntityAI current = player.GetHumanInventory().GetEntityInHands();
		if (current)
		{
			// The slot only frees after this frame: drop now, create on the next tick.
			string dropped = current.GetType();
			player.ServerDropEntity(current);
			string again = "hands";
			for (int i = 1; i < words.Count(); i++)
				again += " " + words[i];
			m_Deferred.Insert(again);
			return "dropped " + dropped + ", creating " + words[1] + " next tick";
		}
		EntityAI item = player.GetHumanInventory().CreateInHands(words[1]);
		if (!item)
			return "cannot create " + words[1] + " in hands";
		Weapon_Base weapon = Weapon_Base.Cast(item);
		if (!weapon)
			return "in hands: " + words[1];
		string magazine = "";
		if (words.Count() > 2)
			magazine = words[2];
		Magazine mag = weapon.SpawnAttachedMagazine(magazine);
		if (!mag)
			return "in hands: " + words[1] + " (no magazine spawned)";
		return "in hands: " + words[1] + " with " + mag.GetType() + " (" + mag.GetAmmoCount().ToString() + " rounds)";
	}

	protected string Spawn(PlayerBase player, array<string> words)
	{
		if (words.Count() < 2)
			return "usage: spawn <class> [dx dy dz]";
		vector offset = player.GetDirection() * 4;
		if (words.Count() >= 5)
			offset = Vector(words[2].ToFloat(), words[3].ToFloat(), words[4].ToFloat());
		vector pos = player.GetPosition() + offset;
		Object object = GetGame().CreateObjectEx(words[1], pos, ECE_PLACE_ON_SURFACE | ECE_CREATEPHYSICS);
		if (!object)
			return "cannot create " + words[1];
		CarScript car = CarScript.Cast(object);
		if (car)
		{
			// Enough parts and fluids to drive straight away; wheel attachment class is
			// <vehicle>Wheel for the vanilla cars, batteries and plugs are shared.
			string wheel = words[1] + "Wheel";
			for (int i = 0; i < 5; i++)
				car.GetInventory().CreateAttachment(wheel);
			car.GetInventory().CreateAttachment("CarBattery");
			car.GetInventory().CreateAttachment("SparkPlug");
			car.GetInventory().CreateAttachment("CarRadiator");
			car.GetInventory().CreateAttachment("HeadlightH7");
			car.Fill(CarFluid.FUEL, 200);
			car.Fill(CarFluid.COOLANT, 20);
			car.Fill(CarFluid.OIL, 20);
			car.Fill(CarFluid.BRAKE, 20);
			return "spawned vehicle " + words[1] + " at " + pos.ToString();
		}
		return "spawned " + words[1] + " at " + pos.ToString();
	}

	protected string Teleport(PlayerBase player, array<string> words)
	{
		vector pos;
		if (words.Count() == 2)
		{
			string preset = words[1];
			preset.ToLower();
			if (preset == "nwaf") pos = "4500 0 10200";
			else if (preset == "cherno") pos = "6600 0 2500";
			else if (preset == "elektro") pos = "10400 0 2200";
			else if (preset == "berezino") pos = "12200 0 9200";
			else if (preset == "svetlo") pos = "13900 0 13200";
			else if (preset == "tisy") pos = "1700 0 14000";
			else if (preset == "zeleno") pos = "2700 0 5300";
			else return "unknown preset " + preset;
		}
		else if (words.Count() == 3)
			pos = Vector(words[1].ToFloat(), 0, words[2].ToFloat());
		else if (words.Count() >= 4)
			pos = Vector(words[1].ToFloat(), words[2].ToFloat(), words[3].ToFloat());
		else
			return "usage: tp <x> <z> | tp <x> <y> <z> | tp <preset>";
		if (pos[1] == 0)
			pos[1] = GetGame().SurfaceY(pos[0], pos[2]) + 0.5;
		player.SetPosition(pos);
		return "teleported to " + pos.ToString();
	}

	protected string Heal(PlayerBase player)
	{
		player.SetHealth("", "Health", player.GetMaxHealth("", "Health"));
		player.SetHealth("", "Blood", player.GetMaxHealth("", "Blood"));
		player.SetHealth("", "Shock", player.GetMaxHealth("", "Shock"));
		player.GetStatEnergy().Set(player.GetStatEnergy().GetMax());
		player.GetStatWater().Set(player.GetStatWater().GetMax());
		if (player.GetBleedingManagerServer())
			player.GetBleedingManagerServer().RemoveAllSources();
		return "healed";
	}

	protected string SetTime(array<string> words)
	{
		if (words.Count() < 2)
			return "usage: time <hour> [minute]";
		int minute = 0;
		if (words.Count() > 2)
			minute = words[2].ToInt();
		int year, month, day, hour, dummy;
		GetGame().GetWorld().GetDate(year, month, day, hour, dummy);
		GetGame().GetWorld().SetDate(year, month, day, words[1].ToInt(), minute);
		return "time set to " + words[1] + ":" + minute.ToString();
	}

	protected string SetWeather(array<string> words)
	{
		if (words.Count() < 2)
			return "usage: weather clear|rain|fog";
		Weather weather = GetGame().GetWeather();
		string kind = words[1];
		kind.ToLower();
		float overcast = 0, rain = 0, fog = 0;
		if (kind == "rain") { overcast = 1; rain = 1; }
		else if (kind == "fog") { overcast = 0.6; fog = 0.8; }
		else if (kind != "clear") return "unknown weather " + kind;
		weather.GetOvercast().Set(overcast, 0, 3600);
		weather.GetRain().Set(rain, 0, 3600);
		weather.GetFog().Set(fog, 0, 3600);
		return "weather " + kind;
	}
}

modded class MissionServer
{
	protected ref DayZVRServerCmd m_DayZVRCmd;

	override void OnInit()
	{
		super.OnInit();
		MakeDirectory("$profile:dayzvr");
		m_DayZVRCmd = new DayZVRServerCmd();
		Print("[DayZVR] server command channel started ($profile:dayzvr/cmd.txt)");
	}

	override void OnUpdate(float timeslice)
	{
		super.OnUpdate(timeslice);
		if (m_DayZVRCmd)
			m_DayZVRCmd.Tick(timeslice);
	}
}
