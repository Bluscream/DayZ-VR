// DayZ VR bridge: client-side Enforce Script half of the native<->script bridge.
// Packed into @DayZVR/addons/DayZVR.pbo by scripts/build-pbo.py (prefix "DayZVR").
class CfgPatches
{
	class DayZVR
	{
		units[] = {};
		weapons[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = {"DZ_Data", "DZ_Scripts"};
	};
};

class CfgMods
{
	class DayZVR
	{
		dir = "DayZVR";
		name = "DayZ VR bridge";
		author = "Bluscream";
		type = "mod";
		dependencies[] = {"Mission"};

		class defs
		{
			class missionScriptModule
			{
				value = "";
				files[] = {"DayZVR/scripts/5_Mission"};
			};
		};
	};
};
