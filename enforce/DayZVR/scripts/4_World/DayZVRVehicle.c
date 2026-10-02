// Vehicle experiments for the VR mod (TASKS V2). Car.SetSteering/SetThrottle are
// script natives; this file tests whether a value written from the client every
// frame survives the engine's own driver input, which decides whether VR steering
// (controller "wheel") can be done in script or needs a native patch.
//
// DayZVRSteering.s_Override: -2 = off, otherwise the wanted steering in <-1, 1>.
// Set from the bridge's client command "steer <value>|off"; the host may later feed
// it from vr.txt. game.txt reports "steering=" (GetSteering) and "speed=" (km/h).
class DayZVRSteering
{
	static float s_Override = -2;
	static float s_Applied = 0;

	static bool Active()
	{
		return s_Override >= -1 && s_Override <= 1;
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
		if (!DayZVRSteering.Active())
			return;
		Human driver = CrewDriver();
		if (!driver || driver != GetGame().GetPlayer())
			return;
		SetSteering(DayZVRSteering.s_Override);
		DayZVRSteering.s_Applied = GetSteering();
	}
}
