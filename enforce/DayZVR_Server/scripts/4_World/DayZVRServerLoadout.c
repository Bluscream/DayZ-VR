// Full test loadout for the local server: night-vision helmet, military clothing,
// plate carrier, large backpack, a rifle with optic and suppressor, a pistol, melee
// weapons, lights, navigation and medical items, applied with the "loadout" command
// (scripts/dayz-cmd.sh loadout). Items that fail to create are listed in the result,
// the rest of the kit still arrives.
class DayZVRServerLoadout
{
	static string Equip(PlayerBase player)
	{
		string report = "";
		player.RemoveAllItems();

		EntityAI helmet = Wear(player, "Mich2001Helmet", report);
		EntityAI goggles = Attach(helmet, "NVGoggles", report);
		Attach(goggles, "Battery9V", report);
		Wear(player, "TacticalShirt_Black", report);
		Wear(player, "GorkaPants_Summer", report);
		Wear(player, "CombatBoots_Black", report);
		Wear(player, "TacticalGloves_Black", report);
		Wear(player, "BalaclavaMask_Black", report);
		EntityAI vest = Wear(player, "PlateCarrierVest", report);
		Attach(vest, "PlateCarrierHolster", report);
		Attach(vest, "PlateCarrierPouches", report);
		Wear(player, "AliceBag_Black", report);
		EntityAI belt = Wear(player, "MilitaryBelt", report);
		Attach(belt, "Canteen", report);
		Attach(belt, "NylonKnifeSheath", report);
		EntityAI headtorch = Attach(helmet, "Headtorch_Black", report);
		Attach(headtorch, "Battery9V", report);

		EntityAI rifle = Hands(player, "M4A1", report);
		Attach(rifle, "M4_RISHndgrd_Black", report);
		Attach(rifle, "M4_MPBttstck_Black", report);
		Attach(rifle, "ACOGOptic", report);
		Attach(rifle, "M4_Suppressor", report);
		Attach(rifle, "UniversalLight", report);
		Attach(rifle, "Mag_STANAG_60Rnd", report);
		Carry(player, "Mag_STANAG_60Rnd", 3, report);

		EntityAI pistol = Carry(player, "FNX45", 1, report);
		Attach(pistol, "PistolSuppressor", report);
		Attach(pistol, "FNP45_MRDSOptic", report);
		Attach(pistol, "Mag_FNX45_15Rnd", report);
		Carry(player, "Mag_FNX45_15Rnd", 2, report);
		Carry(player, "Machete", 1, report);
		Carry(player, "FirefighterAxe", 1, report);
		Carry(player, "CombatKnife", 1, report);

		Carry(player, "Rangefinder", 1, report);
		Carry(player, "Binoculars", 1, report);
		Carry(player, "Compass", 1, report);
		Carry(player, "ChernarusMap", 1, report);
		EntityAI gps = Carry(player, "GPSReceiver", 1, report);
		Attach(gps, "Battery9V", report);
		EntityAI radio = Carry(player, "PersonalRadio", 1, report);
		Attach(radio, "Battery9V", report);
		EntityAI flashlight = Carry(player, "Flashlight", 1, report);
		Attach(flashlight, "Battery9V", report);
		Carry(player, "Battery9V", 3, report);
		Carry(player, "Chemlight_Green", 2, report);
		Carry(player, "Roadflare", 2, report);
		Carry(player, "Matchbox", 1, report);

		Carry(player, "BandageDressing", 4, report);
		Carry(player, "Morphine", 2, report);
		Carry(player, "Epinephrine", 2, report);
		Carry(player, "SalineBagIV", 1, report);
		Carry(player, "TetracyclineAntibiotics", 1, report);
		Carry(player, "SewingKit", 1, report);
		Carry(player, "DuctTape", 1, report);
		Carry(player, "WaterPurificationTablets", 1, report);
		Carry(player, "TunaCan", 3, report);
		Carry(player, "Canteen", 1, report);

		if (report == "")
			return "loadout equipped";
		return "loadout equipped, failed:" + report;
	}

	protected static EntityAI Wear(PlayerBase player, string type, out string report)
	{
		EntityAI item = player.GetInventory().CreateInInventory(type);
		if (!item)
			report += " " + type;
		return item;
	}

	protected static EntityAI Hands(PlayerBase player, string type, out string report)
	{
		EntityAI item = player.GetHumanInventory().CreateInHands(type);
		if (!item)
			report += " " + type;
		return item;
	}

	protected static EntityAI Attach(EntityAI parent, string type, out string report)
	{
		if (!parent)
			return null;
		EntityAI item = parent.GetInventory().CreateAttachment(type);
		if (!item)
			report += " " + type + "@" + parent.GetType();
		return item;
	}

	// Into any free inventory space; what does not fit lands at the player's feet.
	protected static EntityAI Carry(PlayerBase player, string type, int count, out string report)
	{
		EntityAI last;
		for (int index = 0; index < count; index++)
		{
			EntityAI item = player.GetInventory().CreateInInventory(type);
			if (!item)
				item = EntityAI.Cast(GetGame().CreateObjectEx(type, player.GetPosition(), ECE_PLACE_ON_SURFACE));
			if (!item)
				report += " " + type;
			else
				last = item;
		}
		return last;
	}
}
