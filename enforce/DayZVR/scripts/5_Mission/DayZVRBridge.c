// Native <-> script bridge. Enforce Script has no sockets or FFI, so both sides
// exchange small "key=value" text files under $profile:dayzvr/ :
//   game.txt  written here every DAYZVR_INTERVAL seconds, read by dxgi.dll
//   vr.txt    written by dxgi.dll (HMD/controller poses, lock states), read here
// Other scripts read the latest VR values through DayZVRBridge.Get().Vr("key").
class DayZVRBridge
{
	static const string DIR = "$profile:dayzvr/";
	static const float DAYZVR_INTERVAL = 0.1;
	static ref DayZVRBridge s_Instance;

	protected float m_Accumulated;
	protected int m_Frame;
	protected ref map<string, string> m_Vr = new map<string, string>();

	static DayZVRBridge Get()
	{
		return s_Instance;
	}

	string Vr(string key)
	{
		string value;
		if (m_Vr.Find(key, value))
			return value;
		return "";
	}

	float VrFloat(string key)
	{
		string value = Vr(key);
		if (value == "")
			return 0;
		return value.ToFloat();
	}

	void Tick(float timeslice)
	{
		m_Accumulated += timeslice;
		if (m_Accumulated < DAYZVR_INTERVAL)
			return;
		m_Accumulated = 0;
		m_Frame++;
		WriteGame();
		ReadVr();
		RunClientCommands();
	}

	protected void WriteGame()
	{
		PlayerBase player = PlayerBase.Cast(GetGame().GetPlayer());
		if (!player)
			return;
		FileHandle file = OpenFile(DIR + "game.txt", FileMode.WRITE);
		if (!file)
			return;
		FPrintln(file, "frame=" + m_Frame.ToString());
		Weapon_Base weapon = Weapon_Base.Cast(player.GetItemInHands());
		if (weapon)
		{
			int muzzle = weapon.GetCurrentMuzzle();
			int ammo = -1;
			Magazine magazine = weapon.GetMagazine(muzzle);
			if (magazine)
				ammo = magazine.GetAmmoCount();
			else if (weapon.HasInternalMagazine(muzzle))
				ammo = weapon.GetInternalMagazineCartridgeCount(muzzle); // bolt-actions, shotguns
			FPrintln(file, "weapon=" + weapon.GetType());
			FPrintln(file, "ammo=" + ammo.ToString());
			FPrintln(file, "chamber=" + BoolText(weapon.IsChamberFull(muzzle)));
		}
		else
		{
			FPrintln(file, "weapon=");
			FPrintln(file, "ammo=-1");
			FPrintln(file, "chamber=0");
		}
		// Health/blood values live on the server; the client only has the synced
		// injury level (eInjuryHandlerLevels: 0 pristine .. 4 ruined) and bleeding bits.
		FPrintln(file, "health_level=" + player.m_HealthLevel.ToString());
		FPrintln(file, "bleeding=" + player.GetBleedingBits().ToString());
		StaminaHandler stamina = player.GetStaminaHandler();
		if (stamina)
			FPrintln(file, "stamina=" + stamina.GetStamina().ToString());
		FPrintln(file, "inventory_open=" + BoolText(GetGame().IsInventoryOpen()));
		HumanMovementState movement = new HumanMovementState();
		player.GetMovementState(movement);
		FPrintln(file, "stance=" + movement.m_iStanceIdx.ToString());
		FPrintln(file, "raised=" + BoolText(movement.IsRaised()));
		FPrintln(file, "in_vehicle=" + BoolText(player.IsInVehicle()));
		// Fists (nothing in hands) or a melee weapon: the native side may turn controller
		// swings into attacks ([melee] motion_swing).
		EntityAI inHands = player.GetItemInHands();
		FPrintln(file, "melee=" + BoolText(!inHands || inHands.IsMeleeWeapon()));
		FPrintln(file, "ammo_label=" + BoolText(DayZVRAmmoCounter.s_Visible) + " " + DayZVRAmmoCounter.s_LastScreen.ToString(false));
		CloseFile(file);
	}

	protected void ReadVr()
	{
		FileHandle file = OpenFile(DIR + "vr.txt", FileMode.READ);
		if (!file)
			return;
		string line;
		while (FGets(file, line) > 0)
		{
			int separator = line.IndexOf("=");
			if (separator <= 0)
				continue;
			m_Vr.Set(line.Substring(0, separator), line.Substring(separator + 1, line.Length() - separator - 1));
		}
		CloseFile(file);
	}

	// Test hooks for the host (scripts/dayz-cmd.sh --client): lines in
	// $profile:dayzvr/client_cmd.txt, results appended to client_cmd.log.
	//   raise <0|1>        hold the weapon raised (OverrideRaise ENABLED/DISABLED)
	//   print <text>       echo into script.log
	protected void RunClientCommands()
	{
		if (!FileExist(DIR + "client_cmd.txt"))
			return;
		array<string> lines = new array<string>();
		FileHandle file = OpenFile(DIR + "client_cmd.txt", FileMode.READ);
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
		DeleteFile(DIR + "client_cmd.txt");
		FileHandle log = OpenFile(DIR + "client_cmd.log", FileMode.APPEND);
		foreach (string command : lines)
		{
			string result = ExecuteClientCommand(command);
			Print("[DayZVR] client cmd: " + command + " -> " + result);
			if (log)
				FPrintln(log, command + " -> " + result);
		}
		if (log)
			CloseFile(log);
	}

	protected string ExecuteClientCommand(string command)
	{
		array<string> words = new array<string>();
		command.Split(" ", words);
		if (words.Count() == 0)
			return "empty";
		PlayerBase player = PlayerBase.Cast(GetGame().GetPlayer());
		if (!player)
			return "no player";
		HumanInputController hic = player.GetInputController();
		string verb = words[0];
		verb.ToLower();
		if (verb == "raise" && words.Count() > 1)
		{
			bool on = words[1] == "1";
			if (on)
				hic.OverrideRaise(HumanInputControllerOverrideType.ENABLED, true);
			else
				hic.OverrideRaise(HumanInputControllerOverrideType.DISABLED, false);
			return "raise " + words[1];
		}
		if (verb == "print")
			return "printed";
		return "unknown client command";
	}

	protected static string BoolText(bool value)
	{
		if (value)
			return "1";
		return "0";
	}
}

modded class MissionGameplay
{
	override void OnInit()
	{
		super.OnInit();
		MakeDirectory("$profile:dayzvr");
		DayZVRBridge.s_Instance = new DayZVRBridge();
		Print("[DayZVR] bridge started");
	}

	override void OnUpdate(float timeslice)
	{
		super.OnUpdate(timeslice);
		if (DayZVRBridge.s_Instance)
			DayZVRBridge.s_Instance.Tick(timeslice);
	}
}
