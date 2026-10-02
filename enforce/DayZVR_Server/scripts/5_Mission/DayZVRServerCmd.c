// Test-server command channel. Every 0.5 s the mission reads $profile:dayzvr/cmd.txt,
// executes each line against the first connected player, deletes the file and appends
// results to $profile:dayzvr/cmd.log (read back by scripts/dayz-cmd.sh).
//
//   give <class> [count]       item(s) into the inventory (ground if full), count <= 50
//   info                       position, direction, alive, vehicle, held item
//   hands <class> [magclass] [rounds]   weapon into hands with an attached magazine
//                              (default magazine from the weapon config, full unless
//                              rounds is given; "-" keeps the default class), or any item;
//                              a held item is dropped first and the creation runs
//                              on the next tick (two "->" lines in cmd.log)
//   spawn <class> [dx dy dz]   object in front of the player (vehicles get wheels,
//                              battery, spark plug, radiator, fuel, water, oil)
//   tp <x> <z> | tp <x> <y> <z> | tp <preset>   teleport (presets: nwaf, cherno,
//                              elektro, berezino, svetlo, tisy, zeleno)
//   tpto [dx dy dz]            stand beside the nearest vehicle (model-space offset,
//                              default -1.6 0 0.3 = driver's door)
//   enter                      seat the player as driver of the nearest vehicle (<= 15 m)
//   exit                       leave the current vehicle (run the client exit too)
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
			if (line == "")
				continue;
			if (line.Length() > MAX_LINE)
			{
				Log(line.Substring(0, 32) + "... -> rejected: longer than " + MAX_LINE.ToString() + " characters");
				continue;
			}
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

	static const int MAX_LINE = 200;
	static const int MAX_WORDS = 8;

	// Class names reach CreateObjectEx/CreateInHands; an unknown one would only log
	// an engine error, so check the config first.
	static bool KnownClass(string name)
	{
		return name.Length() <= 64 && GetGame().ConfigIsExisting("CfgVehicles " + name);
	}

	protected string Execute(string command)
	{
		array<string> words = new array<string>();
		command.Split(" ", words);
		if (words.Count() == 0)
			return "empty";
		if (words.Count() > MAX_WORDS)
			return "too many arguments (max " + MAX_WORDS.ToString() + ")";
		string verb = words[0];
		verb.ToLower();
		PlayerBase player = FirstPlayer();
		if (!player)
			return "no player connected";
		if ((verb == "give" || verb == "hands" || verb == "spawn") && words.Count() > 1 && !KnownClass(words[1]))
			return "unknown class " + words[1];
		if (verb == "give")
			return Give(player, words);
		if (verb == "hands")
			return Hands(player, words);
		if (verb == "spawn")
			return Spawn(player, words);
		if (verb == "tp")
			return Teleport(player, words);
		if (verb == "tpto")
			return TeleportToVehicle(player, words);
		if (verb == "enter")
			return EnterVehicle(player);
		if (verb == "exit")
		{
			if (!player.GetCommand_Vehicle())
				return "not in a vehicle";
			player.m_DayZVRServerGetOut = true;
			return "leaving the vehicle on the next CommandHandler tick";
		}
		if (verb == "info")
		{
			string info = "pos=" + player.GetPosition().ToString() + " dir=" + player.GetDirection().ToString();
			info += " alive=" + player.IsAlive().ToString() + " in_vehicle=" + player.IsInVehicle().ToString();
			EntityAI held = player.GetHumanInventory().GetEntityInHands();
			if (held)
				info += " hands=" + held.GetType();
			return info;
		}
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
			count = Math.Clamp(words[2].ToInt(), 1, 50);
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
			return "usage: hands <class> [magclass|-] [rounds]";
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
		int muzzle = weapon.GetCurrentMuzzle();
		if (weapon.HasInternalMagazine(muzzle))
		{
			// Bolt-actions, shotguns: SpawnAttachedMagazine throws for these (no
			// "magazines" config entry), so fill the internal magazine instead; "rounds"
			// cannot be set for it.
			weapon.FillInnerMagazine("", WeaponWithAmmoFlags.CHAMBER);
			return "in hands: " + words[1] + " with internal magazine (" + weapon.GetInternalMagazineCartridgeCount(muzzle).ToString() + " rounds)";
		}
		string magazine = "";
		if (words.Count() > 2 && words[2] != "-")
			magazine = words[2];
		Magazine mag = weapon.SpawnAttachedMagazine(magazine);
		if (!mag)
			return "in hands: " + words[1] + " (no magazine spawned)";
		if (words.Count() > 3)
			mag.ServerSetAmmoCount(words[3].ToInt());
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
			// Enough parts and fluids to drive straight away. Wheel classes follow no
			// single pattern (4_World/Entities/Vehicles/InheritedCars), so map them.
			string wheel = WheelClass(words[1]);
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

	protected static string WheelClass(string vehicle)
	{
		if (vehicle == "OffroadHatchback") return "HatchbackWheel";
		if (vehicle == "CivilianSedan") return "CivSedanWheel";
		if (vehicle.IndexOf("Truck_01") == 0) return "Truck_01_Wheel";
		return vehicle + "_Wheel"; // Hatchback_02, Sedan_02, Offroad_02, Truck_02
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

	// Puts the player beside the driver's door of the nearest vehicle (<= 50 m), so the
	// client's "enter" command passes the vanilla reach check.
	protected string TeleportToVehicle(PlayerBase player, array<string> words)
	{
		vector offset = "-1.6 0 0.3";
		if (words.Count() >= 4)
			offset = Vector(words[1].ToFloat(), words[2].ToFloat(), words[3].ToFloat());
		Transport nearest = NearestVehicle(player, 50);
		if (!nearest)
			return "no vehicle within 50 m";
		// Default: driver's door on the model's left (-x), one metre out from the body.
		vector pos = nearest.ModelToWorld(offset);
		pos[1] = GetGame().SurfaceY(pos[0], pos[2]) + 0.3;
		player.SetPosition(pos);
		return "teleported beside " + nearest.GetType() + " at " + pos.ToString();
	}

	protected Transport NearestVehicle(PlayerBase player, float radius)
	{
		array<Object> objects = new array<Object>();
		array<CargoBase> proxies = new array<CargoBase>();
		GetGame().GetObjectsAtPosition3D(player.GetPosition(), radius, objects, proxies);
		Transport nearest;
		float best = radius * 2;
		foreach (Object object : objects)
		{
			Transport transport = Transport.Cast(object);
			if (!transport)
				continue;
			float distance = vector.Distance(object.GetPosition(), player.GetPosition());
			if (distance < best)
			{
				best = distance;
				nearest = transport;
			}
		}
		return nearest;
	}

	// Server-side seat entry: the same command the vanilla ActionGetInTransport starts
	// (its Start() runs on both machines); here only the server starts it so the
	// test rig can seat the player without a cursor target. V2 experiment.
	protected string EnterVehicle(PlayerBase player)
	{
		Transport vehicle = NearestVehicle(player, 15);
		if (!vehicle)
			return "no vehicle within 15 m";
		if (vehicle.CrewMember(0))
			return "driver seat of " + vehicle.GetType() + " is taken";
		player.m_DayZVRServerGetIn = vehicle;
		return "vehicle command queued for " + vehicle.GetType() + " (driver seat, next CommandHandler tick)";
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
