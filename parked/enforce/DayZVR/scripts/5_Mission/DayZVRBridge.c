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
	protected string m_VrFrame;
	protected float m_VrFrameTime; // GetGame().GetTime() ms when vr.txt last changed

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

	// True while the native side keeps writing vr.txt (frame changed within maxAgeMs).
	bool VrFresh(float maxAgeMs = 1000)
	{
		return m_VrFrame != "" && GetGame().GetTime() - m_VrFrameTime <= maxAgeMs;
	}

	float VrFloat(string key)
	{
		string value = Vr(key);
		if (value == "")
			return 0;
		return value.ToFloat();
	}

	protected ref DayZVRDashboard m_Dashboard;

	void Tick(float timeslice)
	{
		m_Accumulated += timeslice;
		if (m_Accumulated < DAYZVR_INTERVAL)
			return;
		m_Accumulated = 0;
		m_Frame++;
		WriteGame();
		ReadVr();
		if (!m_Dashboard)
			m_Dashboard = new DayZVRDashboard();
		m_Dashboard.Update(this);
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
		CarScript car = CarScript.Cast(player.GetParent());
		if (car)
		{
			FPrintln(file, "steering=" + car.GetSteering().ToString());
			FPrintln(file, "speed=" + car.GetSpeedometer().ToString());
			FPrintln(file, "driver=" + BoolText(car.CrewDriver() == player));
			// Dashboard data for a wrist/dash widget (V2).
			FPrintln(file, "gear=" + car.GetGear().ToString());
			FPrintln(file, "rpm=" + car.EngineGetRPM().ToString());
			FPrintln(file, "engine=" + BoolText(car.EngineIsOn()));
			FPrintln(file, "lights=" + BoolText(car.IsScriptedLightsOn()));
			FPrintln(file, "fuel=" + car.GetFluidFraction(CarFluid.FUEL).ToString());
		}
		// Fists (nothing in hands) or a melee weapon: the native side may turn controller
		// swings into attacks ([melee] motion_swing).
		EntityAI inHands = player.GetItemInHands();
		FPrintln(file, "melee=" + BoolText(!inHands || inHands.IsMeleeWeapon()));
		FPrintln(file, "ammo_label=" + BoolText(DayZVRAmmoCounter.s_Visible) + " " + DayZVRAmmoCounter.s_LastScreen.ToString(false));
		FPrintln(file, "dashboard=" + BoolText(DayZVRDashboard.s_Visible) + " " + DayZVRDashboard.s_LastText);
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
		string frame = Vr("frame");
		if (frame != m_VrFrame)
		{
			m_VrFrame = frame;
			m_VrFrameTime = GetGame().GetTime();
		}
		// Hand the steering wheel value to the World module (see DayZVRVehicle.c).
		DayZVRSteering.s_VrValid = Vr("steer_valid") == "1";
		DayZVRSteering.s_VrSteer = VrFloat("steer");
		DayZVRSteering.s_VrTime = m_VrFrameTime;
		DayZVRSteering.s_VrPedalsValid = Vr("pedals_valid") == "1";
		DayZVRSteering.s_VrThrottle = VrFloat("throttle");
		DayZVRSteering.s_VrBrake = VrFloat("brake");
		DayZVRSteering.s_VrButtonA = Vr("btn_a") == "1";
		DayZVRSteering.s_VrStickClickR = Vr("stick_click_r") == "1";
		DayZVRSteering.s_VrGrabL = VrFloat("grab_l");
	}

	// Test hooks for the host (scripts/dayz-cmd.sh --client): lines in
	// $profile:dayzvr/client_cmd.txt, results appended to client_cmd.log.
	//   raise <0|1>        hold the weapon raised (OverrideRaise ENABLED/DISABLED)
	//   fire               pull the trigger once through WeaponManager.Fire (test aid)
	//   engine|lights|horn what the in-car A / grip+A / right stick click do (test aid)
	//   enter              get into the driver seat of the nearest vehicle (<= 15 m)
	//                      (vehicle command queued for the CommandHandler tick)
	//   exit               leave the current vehicle (same mechanism)
	//   steer <v>|off      script steering override while driving (V2 experiment)
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
			if (line == "" || line.Length() > 200)
				continue;
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
		if (words.Count() > 8)
			return "too many arguments (max 8)";
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
		if (verb == "enter")
			return EnterNearestVehicle(player);
		if (verb == "exit")
		{
			if (!player.GetCommand_Vehicle())
				return "not in a vehicle";
			player.m_DayZVRGetOut = true;
			return "leaving the vehicle on the next CommandHandler tick; run the server 'exit' too";
		}
		if (verb == "fire")
		{
			Weapon_Base weapon = Weapon_Base.Cast(player.GetItemInHands());
			if (!weapon)
				return "no firearm in hands";
			if (!player.GetWeaponManager().CanFire(weapon))
				return "cannot fire now (raise it first, wait for the raise to finish)";
			player.GetWeaponManager().Fire(weapon);
			return "fired " + weapon.GetType();
		}
		if (verb == "engine" || verb == "lights" || verb == "horn")
		{
			HumanCommandVehicle vehicleCommand = player.GetCommand_Vehicle();
			CarScript car;
			if (!vehicleCommand || !Class.CastTo(car, vehicleCommand.GetTransport()))
				return "not in a car";
			car.DayZVRTestButton(player, verb);
			return verb + " -> engine_on=" + car.EngineIsOn().ToString();
		}
		if (verb == "steer" && words.Count() > 1)
		{
			if (words[1] == "off")
				DayZVRSteering.s_Override = -2;
			else
				DayZVRSteering.s_Override = Math.Clamp(words[1].ToFloat(), -1, 1);
			return "steer " + words[1];
		}
		if (verb == "print")
			return "printed";
		return "unknown client command";
	}

	protected string EnterNearestVehicle(PlayerBase player)
	{
		array<Object> objects = new array<Object>();
		array<CargoBase> proxies = new array<CargoBase>();
		GetGame().GetObjectsAtPosition3D(player.GetPosition(), 15, objects, proxies);
		Transport nearest;
		float best = 1000;
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
		if (!nearest)
			return "no vehicle within 15 m";
		ActionManagerClient manager = ActionManagerClient.Cast(player.GetActionManager());
		if (!manager)
			return "no client action manager";
		ActionBase action = manager.GetAction(ActionGetInTransport);
		if (!action)
			return "ActionGetInTransport unavailable";
		// The action wants the component index of a door selection. Try every component
		// that maps to the driver's crew position (0) until the vanilla condition
		// (reach, door free, seat empty) accepts one.
		string tried = "";
		array<string> selectionsForHit = new array<string>();
		for (int i = 0; i < 256; i++)
		{
			if (nearest.CrewPositionIndex(i) != 0)
				continue;
			// The cursor condition measures from the hit position, so aim it at the seat.
			vector hit = nearest.GetPosition();
			if (selectionsForHit.Count() == 0)
				nearest.GetActionComponentNameList(i, selectionsForHit);
			if (selectionsForHit.Count() > 0)
				hit = nearest.GetSelectionPositionWS(selectionsForHit[0]);
			selectionsForHit.Clear();
			ActionTarget target = new ActionTarget(nearest, null, i, hit, 0);
			if (action.Can(player, target, null))
			{
				// PerformActionStart from here never produced a visible entry (the request
				// needs the real input path); start the command the way vanilla's
				// DEVELOPER debug does and let the server command "enter" do its half.
				player.m_DayZVRGetIn = nearest;
				return "vehicle command queued (action condition ok, component " + i.ToString() + ") on " + nearest.GetType() + "; run the server 'enter' too";
			}
			// Spell out the vanilla ActionCondition so the failing check is visible.
			array<string> selections = new array<string>();
			nearest.GetActionComponentNameList(i, selections);
			tried += " " + i.ToString() + "(";
			foreach (string selection : selections)
				tried += selection + " reach=" + BoolText(nearest.CanReachSeatFromDoors(selection, player.GetPosition(), 1.0)) + ",";
			tried += " through=" + BoolText(nearest.CrewCanGetThrough(0));
			tried += " doorfree=" + BoolText(nearest.IsAreaAtDoorFree(0));
			tried += " seatfree=" + BoolText(!nearest.CrewMember(0));
			tried += " heavy=" + BoolText(player.GetItemInHands() && player.GetItemInHands().IsHeavyBehaviour());
			tried += " invehicle=" + BoolText(player.GetCommand_Vehicle() != null);
			tried += ")";
		}
		if (tried == "")
			return "no driver door component on " + nearest.GetType();
		// The vanilla action's Start() runs StartCommand_Vehicle on both machines; when
		// the action cannot be requested (no cursor target in the test rig), start the
		// command locally and let the server command "enter" do its half.
		player.m_DayZVRGetIn = nearest;
		return "vehicle command queued for the next CommandHandler tick on " + nearest.GetType() + " (action refused:" + tried + "); run the server 'enter' too";
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
