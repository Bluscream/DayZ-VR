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
	// Last computed label position and visibility, reported in game.txt for the host.
	static vector s_LastScreen;
	static bool s_Visible;
	// Offset of the label from the aim point (screen fractions). The GUI is squeezed
	// into the HUD content rectangle (hud_scale/safe area, bottom at ~0.8 of the frame),
	// so the drawn magazine (~0.95) is unreachable; the label clamps to the HUD's
	// bottom-right instead, which is as close to the gun as GUI space allows.
	static const float LABEL_DX = 0.09;
	static const float LABEL_DY = 0.30;

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
		m_Text.SetTextExactSize(48);
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
		if (magazine)
			ammo = magazine.GetAmmoCount();
		else
		{
			// Internal magazines (bolt rifles, shotguns) have no attached entity.
			ammo = weapon.GetInternalMagazineCartridgeCount(muzzle);
			if (weapon.IsChamberFull(muzzle) && !weapon.IsChamberFiredOut(muzzle))
				ammo = ammo + 1;
		}
		HumanMovementState movement = new HumanMovementState();
		player.GetMovementState(movement);
		if (!movement.IsRaised())
		{
			// Lowered weapon: the first-person model is mostly out of view.
			Hide();
			return;
		}
		// The first-person weapon is a separate hands model drawn relative to the camera;
		// the world entity (what GetPosition/ModelToWorld report) sits at the body and
		// projects nowhere near the drawn gun. So the label is placed at a fixed offset
		// from the aim point instead: below-right of centre, where the raised rifle's
		// magazine is drawn. The aim point itself moves with the view offset the native
		// side reports (controller aim or locked axes).
		float vfov = GetGame().GetUserFOV();
		if (vfov <= 0)
			vfov = 0.75;
		int sw, sh;
		GetScreenSize(sw, sh);
		float hfov = vfov * sw / sh;
		vector screen = "0.5 0.5 1";
		screen[0] = 0.5 + bridge.VrFloat("view_yaw_offset") / hfov + LABEL_DX;
		screen[1] = 0.5 - bridge.VrFloat("view_pitch_offset") / vfov + LABEL_DY;
		// Widget coordinates span the HUD content rectangle the native side squeezes the
		// GUI into (hud_scale / safe area), not the full frame: map accordingly.
		float hudW = bridge.VrFloat("hud_width");
		float hudH = bridge.VrFloat("hud_height");
		if (hudW > 0.01 && hudH > 0.01)
		{
			screen[0] = (screen[0] - bridge.VrFloat("hud_left")) / hudW;
			screen[1] = (screen[1] - bridge.VrFloat("hud_top")) / hudH;
		}
		screen[0] = Math.Clamp(screen[0], 0.0, 0.9);
		screen[1] = Math.Clamp(screen[1], 0.0, 0.95);
		s_LastScreen = screen;
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
		m_Text.SetPos(screen[0], screen[1]);
		m_Root.Show(true);
		s_Visible = true;
	}

	protected void Hide()
	{
		s_Visible = false;
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
