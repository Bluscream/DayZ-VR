// DayZ VR test-server mod: executes spawn/teleport/heal commands written to
// $profile:dayzvr/cmd.txt on the server (scripts/dayz-cmd.sh on the host), so the
// VR client can be given weapons, vehicles and positions for feature testing.
// Load with -serverMod=@DayZVR_Server. Not meant for public servers (no auth).
class CfgPatches
{
	class DayZVR_Server
	{
		units[] = {};
		weapons[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = {"DZ_Data", "DZ_Scripts"};
	};
};

class CfgMods
{
	class DayZVR_Server
	{
		dir = "DayZVR_Server";
		name = "DayZ VR test server commands";
		author = "Bluscream";
		type = "mod";
		dependencies[] = {"Mission"};

		class defs
		{
			class missionScriptModule
			{
				value = "";
				files[] = {"DayZVR_Server/scripts/5_Mission"};
			};
		};
	};
};
