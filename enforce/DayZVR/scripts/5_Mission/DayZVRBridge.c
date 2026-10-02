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
