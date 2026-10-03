// Vehicle dashboard: one text line with speed, gear, rpm, fuel, engine and lights,
// drawn in screen space at the bottom of the HUD content rectangle while the local
// player drives a car. The native side composites the HUD layer into the headset, so
// this lands in VR without any native drawing. Enabled from the native side through
// vr.txt (dashboard=1, ini [bridge] dashboard) so one ini controls the whole mod.
// Updated by DayZVRBridge.Tick (ten times a second, enough for a dashboard).
class DayZVRDashboard
{
	protected Widget m_Root;
	protected TextWidget m_Text;
	protected string m_LastText;
	// Last text and visibility, reported in game.txt for the host and the test rig.
	static string s_LastText;
	static bool s_Visible;
	static const float POS_X = 0.5;  // fraction of the HUD rectangle, text centred around it
	static const float POS_Y = 0.9;
	static const int TEXT_SIZE = 40;

	void DayZVRDashboard()
	{
		m_Root = GetGame().GetWorkspace().CreateWidget(FrameWidgetTypeID, 0, 0, 1.0, 1.0,
			WidgetFlags.VISIBLE | WidgetFlags.HEXACTPOS | WidgetFlags.VEXACTPOS, 0xffffffff, 0);
		m_Text = TextWidget.Cast(GetGame().GetWorkspace().CreateWidget(TextWidgetTypeID, 0, 0, 900, 50,
			WidgetFlags.VISIBLE | WidgetFlags.HEXACTSIZE | WidgetFlags.VEXACTSIZE | WidgetFlags.CENTER,
			0xffffffff, 1, m_Root));
		m_Text.SetTextExactSize(TEXT_SIZE);
		m_Text.SetText("");
		m_Root.Show(false);
	}

	void ~DayZVRDashboard()
	{
		if (m_Root)
			m_Root.Unlink();
	}

	static string Compose(CarScript car)
	{
		int speed = Math.Round(Math.AbsFloat(car.GetSpeedometer()));
		int rpm = Math.Round(car.EngineGetRPM() / 100.0) * 100;
		int fuel = Math.Round(car.GetFluidFraction(CarFluid.FUEL) * 100.0);
		// CarGear: 0 reverse, 1 neutral, 2.. forward gears.
		string gear;
		if (car.GetGear() == 0)
			gear = "R";
		else if (car.GetGear() == 1)
			gear = "N";
		else
			gear = (car.GetGear() - 1).ToString();
		string engine = "engine off";
		if (car.EngineIsOn())
			engine = rpm.ToString() + " rpm";
		string lights = "";
		if (car.IsScriptedLightsOn())
			lights = "  lights";
		return speed.ToString() + " km/h   gear " + gear + "   " + engine + "   fuel " + fuel.ToString() + "%" + lights;
	}

	void Update(DayZVRBridge bridge)
	{
		if (!m_Root || !bridge)
			return;
		PlayerBase player = PlayerBase.Cast(GetGame().GetPlayer());
		if (!player || bridge.Vr("dashboard") != "1" || GetGame().IsInventoryOpen() || GetGame().GetUIManager().GetMenu())
		{
			Hide();
			return;
		}
		CarScript car = CarScript.Cast(player.GetParent());
		if (!car || car.CrewDriver() != player)
		{
			Hide();
			return;
		}
		string text = Compose(car);
		if (text != m_LastText)
		{
			m_Text.SetText(text);
			m_LastText = text;
			s_LastText = text;
			if (!car.EngineIsOn())
				m_Text.SetColor(0xffffc040);
			else
				m_Text.SetColor(0xffffffff);
		}
		// Same HUD-rectangle mapping as the ammo label: widget space is the squeezed HUD.
		float x = POS_X;
		float y = POS_Y;
		float hudW = bridge.VrFloat("hud_width");
		float hudH = bridge.VrFloat("hud_height");
		if (hudW > 0.01 && hudH > 0.01)
		{
			x = (x - bridge.VrFloat("hud_left")) / hudW;
			y = (y - bridge.VrFloat("hud_top")) / hudH;
		}
		// The text widget has no HEXACTPOS flag, so SetPos takes screen fractions; the
		// 900 px box is centred by shifting half its width.
		int sw, sh;
		GetScreenSize(sw, sh);
		float halfWidth = 0.25;
		if (sw > 0)
			halfWidth = 450.0 / sw;
		m_Text.SetPos(Math.Clamp(x - halfWidth, 0.0, 1.0), Math.Clamp(y, 0.0, 0.97));
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
