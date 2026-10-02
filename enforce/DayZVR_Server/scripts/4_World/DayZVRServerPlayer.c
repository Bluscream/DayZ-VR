// Server half of the test rig's vehicle entry: the "enter" command parks the
// transport here and the player's CommandHandler tick starts the vehicle command,
// mirroring how the vanilla ActionGetInTransport.Start() runs on the server.
modded class PlayerBase
{
	Transport m_DayZVRServerGetIn;

	override void CommandHandler(float pDt, int pCurrentCommandID, bool pCurrentCommandFinished)
	{
		super.CommandHandler(pDt, pCurrentCommandID, pCurrentCommandFinished);
		if (!m_DayZVRServerGetIn)
			return;
		Transport transport = m_DayZVRServerGetIn;
		m_DayZVRServerGetIn = null;
		int seat = transport.GetSeatAnimationType(0);
		HumanCommandVehicle command = StartCommand_Vehicle(transport, 0, seat);
		if (command)
			command.SetVehicleType(transport.GetAnimInstance());
		Print("[DayZVR] server CommandHandler StartCommand_Vehicle " + transport.GetType() + " -> " + (command != null).ToString());
	}
}
