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
