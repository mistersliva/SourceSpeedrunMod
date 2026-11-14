# Generates the HTML GameUI sound set (16-bit, 44.1 kHz, mono WAV) into this
# directory. Fully procedural and deterministic: re-running produces identical
# files. Usage:  powershell -ExecutionPolicy Bypass -File generate_sounds.ps1

param([string]$OutDir = $PSScriptRoot)

Add-Type -Language CSharp -TypeDefinition @"
using System;
using System.IO;

public static class UiSynth
{
    const int Rate = 44100;

    static float[] Buf(double seconds) { return new float[(int)(seconds * Rate)]; }

    // Attack/decay envelope (exponential decay).
    static double Env(double t, double attack, double decay)
    {
        if (t < attack) return t / attack;
        return Math.Exp(-(t - attack) / decay);
    }

    // Adds a sine sweep from f0 to f1 (exponential glide) with an envelope.
    static void Tone(float[] b, double start, double dur, double f0, double f1, double amp,
                     double attack, double decay, double harm2 = 0, double harm3 = 0)
    {
        int s0 = (int)(start * Rate), n = (int)(dur * Rate);
        double phase = 0;
        for (int i = 0; i < n && s0 + i < b.Length; i++)
        {
            double t = (double)i / Rate, k = (double)i / n;
            double f = f0 * Math.Pow(f1 / f0, k);
            phase += 2 * Math.PI * f / Rate;
            double v = Math.Sin(phase) + harm2 * Math.Sin(2 * phase) + harm3 * Math.Sin(3 * phase);
            b[s0 + i] += (float)(v * amp * Env(t, attack, decay));
        }
    }

    // Band-limited noise burst (one-pole low/high pass), cutoff glides c0 -> c1.
    static void Noise(float[] b, double start, double dur, double c0, double c1, double amp,
                      double attack, double decay, int seed)
    {
        var rng = new Random(seed);
        int s0 = (int)(start * Rate), n = (int)(dur * Rate);
        double lp = 0, hpIn = 0, hp = 0;
        for (int i = 0; i < n && s0 + i < b.Length; i++)
        {
            double t = (double)i / Rate, k = (double)i / n;
            double cut = c0 + (c1 - c0) * k;
            double a = 1 - Math.Exp(-2 * Math.PI * cut / Rate);
            double x = rng.NextDouble() * 2 - 1;
            lp += a * (x - lp);
            // high-pass at cut/4 to keep it airy
            double ah = Math.Exp(-2 * Math.PI * (cut / 4) / Rate);
            hp = ah * (hp + lp - hpIn);
            hpIn = lp;
            b[s0 + i] += (float)(hp * amp * Env(t, attack, decay));
        }
    }

    static void Normalize(float[] b, double peak)
    {
        float m = 1e-6f;
        foreach (var v in b) m = Math.Max(m, Math.Abs(v));
        float g = (float)(peak / m);
        for (int i = 0; i < b.Length; i++) b[i] *= g;
        // 3 ms fade-out to avoid clicks
        int f = Math.Min(b.Length, Rate * 3 / 1000);
        for (int i = 0; i < f; i++) b[b.Length - 1 - i] *= (float)i / f;
    }

    static void Save(string path, float[] b)
    {
        using (var w = new BinaryWriter(File.Create(path)))
        {
            int bytes = b.Length * 2;
            w.Write(new[] { 'R', 'I', 'F', 'F' }); w.Write(36 + bytes);
            w.Write(new[] { 'W', 'A', 'V', 'E' });
            w.Write(new[] { 'f', 'm', 't', ' ' }); w.Write(16); w.Write((short)1); w.Write((short)1);
            w.Write(Rate); w.Write(Rate * 2); w.Write((short)2); w.Write((short)16);
            w.Write(new[] { 'd', 'a', 't', 'a' }); w.Write(bytes);
            foreach (var v in b) w.Write((short)Math.Max(-32767, Math.Min(32767, v * 32767)));
        }
    }

    public static void Generate(string dir)
    {
        float[] b;

        // hover: soft, short glassy tick
        b = Buf(0.06); Tone(b, 0, 0.06, 2600, 2300, 1, 0.001, 0.012, 0.15); Noise(b, 0, 0.01, 9000, 6000, 0.25, 0.0005, 0.003, 1);
        Normalize(b, 0.35); Save(Path.Combine(dir, "hover.wav"), b);

        // click: confirm, two-step rising blip
        b = Buf(0.12); Tone(b, 0, 0.05, 900, 1000, 1, 0.002, 0.03, 0.2); Tone(b, 0.035, 0.08, 1350, 1450, 0.8, 0.002, 0.035, 0.2);
        Noise(b, 0, 0.012, 7000, 4000, 0.3, 0.0005, 0.004, 2);
        Normalize(b, 0.55); Save(Path.Combine(dir, "click.wav"), b);

        // back: closing, falling blip
        b = Buf(0.14); Tone(b, 0, 0.12, 1200, 620, 1, 0.002, 0.045, 0.15);
        Normalize(b, 0.5); Save(Path.Combine(dir, "back.wav"), b);

        // open: panel slides in — airy swish with a rising tone
        b = Buf(0.32); Noise(b, 0, 0.3, 1200, 6500, 1, 0.06, 0.09, 3); Tone(b, 0.02, 0.26, 420, 760, 0.35, 0.05, 0.1, 0.3);
        Normalize(b, 0.45); Save(Path.Combine(dir, "open.wav"), b);

        // tab: light tap
        b = Buf(0.07); Tone(b, 0, 0.06, 1600, 1500, 1, 0.001, 0.018, 0.25);
        Normalize(b, 0.4); Save(Path.Combine(dir, "tab.wav"), b);

        // toggle on / off
        b = Buf(0.08); Tone(b, 0, 0.07, 1100, 1500, 1, 0.001, 0.022, 0.1);
        Normalize(b, 0.45); Save(Path.Combine(dir, "toggle_on.wav"), b);
        b = Buf(0.08); Tone(b, 0, 0.07, 1300, 900, 1, 0.001, 0.022, 0.1);
        Normalize(b, 0.4); Save(Path.Combine(dir, "toggle_off.wav"), b);

        // slider: tiny detent tick
        b = Buf(0.025); Tone(b, 0, 0.02, 3200, 3000, 1, 0.0005, 0.004); Noise(b, 0, 0.006, 10000, 8000, 0.4, 0.0003, 0.0015, 4);
        Normalize(b, 0.25); Save(Path.Combine(dir, "slider.wav"), b);

        // error: two low muted pulses
        b = Buf(0.3); Tone(b, 0, 0.11, 240, 225, 1, 0.003, 0.05, 0.5, 0.3); Tone(b, 0.13, 0.14, 200, 185, 1, 0.003, 0.06, 0.5, 0.3);
        Normalize(b, 0.55); Save(Path.Combine(dir, "error.wav"), b);

        // notify: two-note bell (modals / toasts)
        b = Buf(0.5); Tone(b, 0, 0.45, 988, 988, 0.8, 0.002, 0.16, 0.25, 0.08); Tone(b, 0.09, 0.4, 1480, 1480, 0.7, 0.002, 0.18, 0.2, 0.06);
        Normalize(b, 0.45); Save(Path.Combine(dir, "notify.wav"), b);

        // success: bright major arpeggio (game saved)
        b = Buf(0.7);
        double[] notes = { 659.3, 830.6, 987.8, 1318.5 };
        for (int i = 0; i < notes.Length; i++) Tone(b, i * 0.06, 0.5, notes[i], notes[i], 0.7, 0.002, 0.15 + 0.05 * i, 0.2, 0.05);
        Normalize(b, 0.5); Save(Path.Combine(dir, "success.wav"), b);

        // start: deep, ominous swell for starting / loading a game
        b = Buf(1.6);
        Tone(b, 0, 1.6, 55, 52, 1, 0.35, 0.55, 0.6, 0.35);
        Tone(b, 0, 1.6, 82.5, 79, 0.6, 0.45, 0.5, 0.3);
        Tone(b, 0.1, 1.4, 220, 330, 0.12, 0.6, 0.35);
        Noise(b, 0, 1.5, 300, 2400, 0.5, 0.5, 0.4, 5);
        Normalize(b, 0.7); Save(Path.Combine(dir, "start.wav"), b);

        // quit: descending power-down
        b = Buf(0.6); Tone(b, 0, 0.55, 660, 110, 1, 0.005, 0.2, 0.35, 0.1); Noise(b, 0, 0.4, 3000, 300, 0.25, 0.01, 0.12, 6);
        Normalize(b, 0.5); Save(Path.Combine(dir, "quit.wav"), b);
    }
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null
[UiSynth]::Generate((Resolve-Path $OutDir).Path)
Get-ChildItem $OutDir -Filter *.wav | ForEach-Object { "{0,-16} {1,8} bytes" -f $_.Name, $_.Length }
