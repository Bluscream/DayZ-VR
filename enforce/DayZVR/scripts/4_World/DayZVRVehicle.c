// Vehicle experiments for the VR mod (TASKS V2). Car.SetSteering/SetThrottle are
// script natives; this file tests whether a value written from the client every
// frame survives the engine's own driver input, which decides whether VR steering
// (controller "wheel") can be done in script or needs a native patch.
//
// Steering sources, in priority: the test override (client command "steer <v>|off",
// -2 = off) and the native two-hand wheel from vr.txt (steer_valid=1, steer=, fresh
// within a second). game.txt reports "steering=" (GetSteering) and "speed=" (km/h).
class DayZVRSteering
{
	static float s_Override = -2;
	static float s_Applied = 0;
	// Filled by DayZVRBridge (5_Mission module, which this World module cannot see)
	// from vr.txt: steer_valid, steer and the game time of the last fresh frame.
	static bool s_VrValid = false;
	static float s_VrSteer = 0;
	static float s_VrTime = -100000;

	static bool Active()
	{
		return s_Override >= -1 && s_Override <= 1;
	}

	// Returns true and the wanted steering when any source is live.
	static bool Wanted(out float steering)
	{
		if (Active())
		{
			steering = s_Override;
			return true;
		}
		if (!s_VrValid || GetGame().GetTime() - s_VrTime > 1000)
			return false;
		steering = Math.Clamp(s_VrSteer, -1, 1);
		return true;
	}
}

// Vehicle commands only take effect when started from the player's CommandHandler
// tick (vanilla's DEVELOPER-only TryGetInVehicleDebug does exactly this), so the
// bridge command just parks the transport here and the next tick starts it.
modded class PlayerBase
{
	Transport m_DayZVRGetIn;

	override void CommandHandler(float pDt, int pCurrentCommandID, bool pCurrentCommandFinished)
	{
		super.CommandHandler(pDt, pCurrentCommandID, pCurrentCommandFinished);
		if (!m_DayZVRGetIn)
			return;
		Transport transport = m_DayZVRGetIn;
		m_DayZVRGetIn = null;
		int seat = transport.GetSeatAnimationType(0);
		HumanCommandVehicle command = StartCommand_Vehicle(transport, 0, seat);
		if (command)
			command.SetVehicleType(transport.GetAnimInstance());
		Print("[DayZVR] CommandHandler StartCommand_Vehicle " + transport.GetType() + " -> " + (command != null).ToString());
	}
}

modded class CarScript
{
	override void OnUpdate(float dt)
	{
		super.OnUpdate(dt);
		float wanted;
		if (!DayZVRSteering.Wanted(wanted))
			return;
		Human driver = CrewDriver();
		if (!driver || driver != GetGame().GetPlayer())
			return;
		SetSteering(wanted);
		DayZVRSteering.s_Applied = GetSteering();
	}
}
