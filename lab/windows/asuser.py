"""Run a command in tom's desktop session (interactive scheduled task) and print its output.
   python asuser.py "<cmd.exe command line>" [wait seconds]
The guest agent runs as SYSTEM in session 0, which has no display; graphics checks need the user's session."""
import base64, subprocess, sys

cmd = sys.argv[1]
wait = int(sys.argv[2]) if len(sys.argv) > 2 else 40
ps = r"""
$ProgressPreference = 'SilentlyContinue'
$c = @'
@CMD@ > C:\asuser.txt 2>&1
'@
Set-Content -Encoding ascii C:\asuser.cmd ('@' + $c)
Remove-Item C:\asuser.txt -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute C:\asuser.cmd
$p = New-ScheduledTaskPrincipal -UserId tom -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName asuser -Action $a -Principal $p -Force | Out-Null
Start-ScheduledTask -TaskName asuser
for ($i = 0; $i -lt @WAIT@; $i++) { Start-Sleep 1; if ($i -gt 2 -and (Get-ScheduledTask asuser).State -ne 'Running') { break } }
Get-Content C:\asuser.txt -ErrorAction SilentlyContinue | Select-String -NotMatch ':info:' | ForEach-Object Line
""".replace("@CMD@", cmd).replace("@WAIT@", str(wait))
enc = base64.b64encode(ps.encode("utf-16-le")).decode()
r = subprocess.run([sys.executable, "lab/lab.py", "exec", "win", "--timeout", str(wait + 60), "--",
                    "powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand", enc], timeout=wait + 120)
sys.exit(r.returncode)
