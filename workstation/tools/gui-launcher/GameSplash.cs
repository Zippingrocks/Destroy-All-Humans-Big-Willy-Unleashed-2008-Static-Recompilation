using System.Diagnostics;
using System.Drawing.Drawing2D;

// Vector artwork stays sharp at desktop DPI and ships inside the launcher.
internal sealed class GameSplash : Form
{
    readonly System.Windows.Forms.Timer animation = new() { Interval = 33 };
    readonly Stopwatch clock = Stopwatch.StartNew();
    string status = "Preparing for invasion…";

    public GameSplash()
    {
        Text = "Destroy All Humans! Big Willy Unleashed";
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(800, 450);
        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.CenterScreen;
        DoubleBuffered = true;
        BackColor = Color.FromArgb(8, 23, 29);
        animation.Tick += (_, _) => Invalidate();
        animation.Start();
    }

    public void SetStatus(string value)
    {
        if (status == value) return;
        status = value;
        AccessibleDescription = value;
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.ScaleTransform(ClientSize.Width / 800f, ClientSize.Height / 450f);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
        using var background = new LinearGradientBrush(new Rectangle(0, 0, 800, 450),
            Color.FromArgb(24, 62, 65), Color.FromArgb(5, 15, 24), 35f);
        g.FillRectangle(background, 0, 0, 800, 450);
        using var radar = new Pen(Color.FromArgb(28, 124, 205, 168), 1);
        for (int r = 80; r <= 650; r += 65)
            g.DrawEllipse(radar, 620 - r, 185 - r, r * 2, r * 2);
        using var star = new SolidBrush(Color.FromArgb(100, 193, 226, 203));
        for (int i = 0; i < 48; i++)
            g.FillEllipse(star, (i * 173 + 31) % 800, (i * 97 + 17) % 340, 2, 2);

        // Flying saucer and its searchlight, drawn behind the title.
        using var beam = new LinearGradientBrush(new Rectangle(535, 135, 240, 230),
            Color.FromArgb(65, 155, 242, 173), Color.Transparent, 90f);
        g.FillPolygon(beam, new Point[] { new(621, 135), new(750, 370), new(492, 370) });
        using var hull = new SolidBrush(Color.FromArgb(116, 185, 164));
        using var dome = new SolidBrush(Color.FromArgb(34, 88, 87));
        g.FillEllipse(dome, 587, 85, 70, 50);
        g.FillEllipse(hull, 546, 113, 153, 25);
        using var dark = new SolidBrush(Color.FromArgb(7, 25, 31));
        g.FillEllipse(dark, 562, 127, 121, 10);

        DrawText(g, "DESTROY ALL", 43, 72, 39, Color.FromArgb(218, 236, 218));
        DrawText(g, "HUMANS!", 39, 113, 78, Color.FromArgb(184, 239, 145));
        using var accent = new SolidBrush(Color.FromArgb(239, 165, 76));
        g.FillRectangle(accent, 46, 220, 42, 4);
        DrawText(g, "BIG WILLY UNLEASHED", 43, 240, 30, Color.FromArgb(239, 165, 76));
        DrawText(g, "THE INVASION IS ABOUT TO BEGIN", 46, 291, 12, Color.FromArgb(156, 187, 181));

        using var footer = new SolidBrush(Color.FromArgb(180, 4, 14, 21));
        g.FillRectangle(footer, 0, 359, 800, 91);
        DrawText(g, status, 46, 382, 15, Color.FromArgb(218, 234, 227), false);
        using var track = new SolidBrush(Color.FromArgb(28, 56, 60));
        g.FillRectangle(track, 46, 422, 708, 3);
        // Indeterminate animation; no invented percentage or artificial delay.
        float x = 46 + (float)(clock.Elapsed.TotalSeconds * 170 % 628);
        g.FillRectangle(accent, x, 422, 80, 3);
        using var border = new Pen(Color.FromArgb(53, 87, 87));
        g.DrawRectangle(border, 0, 0, 799, 449);
    }

    static void DrawText(Graphics g, string text, float x, float y, float size, Color color, bool bold = true)
    {
        using var font = new Font("Segoe UI", size, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
        using var brush = new SolidBrush(color);
        g.DrawString(text, font, brush, x, y);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) animation.Dispose();
        base.Dispose(disposing);
    }
}
