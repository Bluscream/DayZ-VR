// Vehicle experiments for the VR mod (TASKS V2). Car.SetSteering/SetThrottle are
// script natives; this file tests whether a value written from the client every
// frame survives the engine's own driver input, which decides whether VR steering
// (controller "wheel") can be done in script or needs a native patch.
//
// Pedals: while driving the host publishes the triggers as throttle (right) and
// brake (left) instead of mouse buttons; applied here only while pressed.
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
	static bool s_VrPedalsValid = false;
	static float s_VrThrottle = 0;
	static float s_VrBrake = 0;
	static const float PEDAL_MIN = 0.02; // below this the keyboard keeps control

	static bool Active()
	{
		return s_Override >= -1 && s_Override <= 1;
	}

	static bool Fresh()
	{
		return GetGame().GetTime() - s_VrTime <= 1000;
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
	bool m_DayZVRGetOut;

	override void CommandHandler(float pDt, int pCurrentCommandID, bool pCurrentCommandFinished)
	{
		super.CommandHandler(pDt, pCurrentCommandID, pCurrentCommandFinished);
		if (m_DayZVRGetOut)
		{
			m_DayZVRGetOut = false;
			HumanCommandVehicle current = GetCommand_Vehicle();
			if (current)
				current.GetOutVehicle();
			Print("[DayZVR] CommandHandler GetOutVehicle -> " + (current != null).ToString());
		}
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
		Human driver = CrewDriver();
		if (!driver || driver != GetGame().GetPlayer())
			return;
		float wanted;
		if (DayZVRSteering.Wanted(wanted))
		{
			SetSteering(wanted);
			DayZVRSteering.s_Applied = GetSteering();
		}
		// Trigger pedals: only while pressed, so W/S keep working with hands off.
		if (DayZVRSteering.s_VrPedalsValid && DayZVRSteering.Fresh())
		{
			if (DayZVRSteering.s_VrThrottle > DayZVRSteering.PEDAL_MIN)
				SetThrottle(Math.Clamp(DayZVRSteering.s_VrThrottle, 0, 1));
			if (DayZVRSteering.s_VrBrake > DayZVRSteering.PEDAL_MIN)
				SetBrake(Math.Clamp(DayZVRSteering.s_VrBrake, 0, 1));
		}
	}
}
