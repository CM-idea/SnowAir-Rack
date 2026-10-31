param(
    [ValidateSet('light', 'dark')]
    [string]$Mode = 'light',
    [int]$Seconds = 20
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
# One or two pixels smaller than the monitor so the rack's fullscreen detector
# doesn't treat this demo backdrop as a fullscreen app.
$form.Bounds = New-Object -TypeName System.Drawing.Rectangle -ArgumentList @(
    $screen.X, $screen.Y, ($screen.Width - 2), ($screen.Height - 2))
$form.ShowInTaskbar = $false
$form.TopMost = $true
$form.Text = 'SnowAir Rack backdrop'

$light1 = [System.Drawing.Color]::FromArgb(238, 244, 252)
$light2 = [System.Drawing.Color]::FromArgb(214, 228, 246)
$dark1 = [System.Drawing.Color]::FromArgb(18, 20, 24)
$dark2 = [System.Drawing.Color]::FromArgb(35, 40, 52)

$form.Add_Paint({
    param($sender, $e)
    $rect = $sender.ClientRectangle
    $c1 = if ($Mode -eq 'dark') { $dark1 } else { $light1 }
    $c2 = if ($Mode -eq 'dark') { $dark2 } else { $light2 }
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 35)
    $e.Graphics.FillRectangle($brush, $rect)
    $brush.Dispose()

    $accent = if ($Mode -eq 'dark') {
        [System.Drawing.Color]::FromArgb(45, 110, 168, 255)
    } else {
        [System.Drawing.Color]::FromArgb(55, 47, 111, 235)
    }
    $accentBrush = New-Object System.Drawing.SolidBrush($accent)
    $e.Graphics.FillEllipse($accentBrush, $rect.Width - 640, -220, 760, 760)
    $e.Graphics.FillEllipse($accentBrush, -260, $rect.Height - 420, 620, 620)
    $accentBrush.Dispose()

    $sub = if ($Mode -eq 'dark') {
        [System.Drawing.Color]::FromArgb(125, 210, 220, 236)
    } else {
        [System.Drawing.Color]::FromArgb(95, 60, 80, 110)
    }
    $font = New-Object System.Drawing.Font('Microsoft YaHei UI', 18)
    $textBrush = New-Object System.Drawing.SolidBrush($sub)
    $e.Graphics.DrawString('SnowAir - Rack', $font, $textBrush, 72, $rect.Height - 120)
    $font.Dispose()
    $textBrush.Dispose()
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = [Math]::Max(1000, $Seconds * 1000)
$timer.Add_Tick({ $form.Close() })
$timer.Start()

[System.Windows.Forms.Application]::Run($form)
$timer.Dispose()
$form.Dispose()
