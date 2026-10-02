// Floating ammo counter: the round count of the magazine in the weapon in hands,
// drawn as a text label projected onto the magazine's position, so it sits next to
// the gun instead of in a HUD corner. Enabled from the native side through vr.txt
// (ammo_counter=1, ini [bridge] ammo_counter) so one ini controls the whole mod.
class DayZVRAmmoCounter
{
	protected Widget m_Root;
	protected TextWidget m_Text;
	protected int m_LastAmmo = -2;
	protected string m_LastText;

	void DayZVRAmmoCounter()
	{
		// A full-screen frame as parent keeps the label in screen space; HEXACTPOS/VEXACTPOS
		// make SetPos take pixels (what GetScreenPos returns).
		m_Root = GetGame().GetWorkspace().CreateWidget(FrameWidgetTypeID, 0, 0, 1.0, 1.0,
			WidgetFlags.VISIBLE | WidgetFlags.HEXACTPOS | WidgetFlags.VEXACTPOS, 0xffffffff, 0);
		// Relative position (0..1 of the screen, what GetScreenPosRelative returns) so
		// the label lands the same on any resolution; the size stays in pixels.
		m_Text = TextWidget.Cast(GetGame().GetWorkspace().CreateWidget(TextWidgetTypeID, 0, 0, 200, 60,
			WidgetFlags.VISIBLE | WidgetFlags.HEXACTSIZE | WidgetFlags.VEXACTSIZE,
			0xffffffff, 1, m_Root));
		m_Text.SetTextExactSize(32);
		m_Text.SetText("");
		m_Root.Show(false);
	}

	void ~DayZVRAmmoCounter()
	{
		if (m_Root)
			m_Root.Unlink();
	}

	// Called every frame (cheap: one projection and a string compare).
	void Update()
	{
		if (!m_Root)
			return;
		PlayerBase player = PlayerBase.Cast(GetGame().GetPlayer());
		DayZVRBridge bridge = DayZVRBridge.Get();
		if (!player || !bridge || bridge.Vr("ammo_counter") != "1" || GetGame().IsInventoryOpen() || GetGame().GetUIManager().GetMenu())
		{
			Hide();
			return;
		}
		Weapon_Base weapon = Weapon_Base.Cast(player.GetItemInHands());
		if (!weapon)
		{
			Hide();
			return;
		}
		int muzzle = weapon.GetCurrentMuzzle();
		Magazine magazine = weapon.GetMagazine(muzzle);
		int ammo = -1;
		vector anchor;
		if (magazine)
		{
			ammo = magazine.GetAmmoCount();
			anchor = magazine.GetPosition();
		}
		else
		{
			// Internal magazines (bolt rifles, shotguns) have no attached entity:
			// count chambered/internal rounds and anchor on the weapon itself.
			ammo = weapon.GetInternalMagazineCartridgeCount(muzzle);
			if (weapon.IsChamberFull(muzzle) && !weapon.IsChamberFiredOut(muzzle))
				ammo = ammo + 1;
			anchor = weapon.GetPosition();
		}
		vector screen = GetGame().GetScreenPosRelative(anchor);
		// z <= 0 means behind the camera (also while the weapon is lowered out of view).
		if (screen[2] <= 0)
		{
			Hide();
			return;
		}
		// The projection uses DayZ's camera; the shown view may point elsewhere
		// (controller aim, locked axes). Shift by the offset the native side reports,
		// scaled by the horizontal/vertical FOV (DayZ's default vertical FOV ~ 0.75 rad).
		float vfov = GetGame().GetUserFOV();
		if (vfov <= 0)
			vfov = 0.75;
		int sw, sh;
		GetScreenSize(sw, sh);
		float hfov = vfov * sw / sh;
		screen[0] = screen[0] + bridge.VrFloat("view_yaw_offset") / hfov;
		screen[1] = screen[1] - bridge.VrFloat("view_pitch_offset") / vfov;
		if (screen[0] < 0 || screen[0] > 1 || screen[1] < 0 || screen[1] > 1)
		{
			Hide();
			return;
		}
		string text = ammo.ToString();
		if (weapon.IsChamberFull(muzzle) && magazine)
			text = text + "+1";
		if (text != m_LastText)
		{
			m_Text.SetText(text);
			m_LastText = text;
			if (ammo <= 0)
				m_Text.SetColor(0xffff4040);
			else if (ammo <= 5)
				m_Text.SetColor(0xffffc040);
			else
				m_Text.SetColor(0xffffffff);
		}
		m_Text.SetPos(screen[0] + 0.012, screen[1] - 0.01);
		m_Root.Show(true);
	}

	protected void Hide()
	{
		if (m_Root.IsVisible())
			m_Root.Show(false);
	}
}

modded class MissionGameplay
{
	protected ref DayZVRAmmoCounter m_DayZVRAmmoCounter;

	override void OnInit()
	{
		super.OnInit();
		m_DayZVRAmmoCounter = new DayZVRAmmoCounter();
	}

	override void OnUpdate(float timeslice)
	{
		super.OnUpdate(timeslice);
		if (m_DayZVRAmmoCounter)
			m_DayZVRAmmoCounter.Update();
	}
}
