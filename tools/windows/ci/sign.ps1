# [CHOSEN, not recovered] Authenticode signing for release CI (docs/ci-releases.md, Windows signing): signs every
# given file that lacks a valid signature with SHA-256, the project's Certum certificate and Certum's RFC 3161
# timestamp, then requires a valid signature on every given file and the project's certificate on each one it
# signed. Files already validly signed (Microsoft's Visual C++ runtime) stay as they are.
# Code/Build/package_release.py --sign-windows certum calls it.
#
# Environment (simplysign.ps1 login sets it): WINDOWS_SIGNTOOL (x64 signtool.exe) and WINDOWS_SIGN_CERT_SHA1 (the
# thumbprint of the certificate SimplySign Desktop registered in the user's store).
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Files)
$ErrorActionPreference = 'Stop'
function Fail([string]$message) { [Console]::Error.WriteLine("sign: $message"); exit 1 }

if (-not $env:WINDOWS_SIGNTOOL -or -not (Test-Path $env:WINDOWS_SIGNTOOL -PathType Leaf)) { Fail 'WINDOWS_SIGNTOOL does not name a file (tools/windows/ci/simplysign.ps1 login)' }
if ($env:WINDOWS_SIGN_CERT_SHA1 -notmatch '^[0-9A-F]{40}$') { Fail 'WINDOWS_SIGN_CERT_SHA1 is not a certificate thumbprint (tools/windows/ci/simplysign.ps1 login)' }
# arrays even for one file: splatting a single string would pass its characters
$paths = @(foreach ($file in $Files) {
  if (-not (Test-Path $file -PathType Leaf)) { Fail "no such file: $file" }
  (Resolve-Path $file).Path
})
$unsigned = @($paths | Where-Object { (Get-AuthenticodeSignature $_).Status -ne 'Valid' })
Write-Output "sign: $($unsigned.Count) of $($paths.Count) file(s) to sign"
if ($unsigned.Count -gt 0) {
  $arguments = @('sign', '/sha1', $env:WINDOWS_SIGN_CERT_SHA1, '/fd', 'SHA256', '/tr', 'http://time.certum.pl', '/td', 'SHA256')
  # the signing cloud and the timestamp server are remote: a failed attempt is retried twice
  for ($attempt = 1; ; ++$attempt) {
    & $env:WINDOWS_SIGNTOOL @arguments @unsigned
    if ($LASTEXITCODE -eq 0) { break }
    if ($attempt -ge 3) { Fail "signtool exited with $LASTEXITCODE" }
    Start-Sleep -Seconds (10 * $attempt)
  }
}
$invalid = @($paths | Where-Object { (Get-AuthenticodeSignature $_).Status -ne 'Valid' })
if ($invalid.Count -gt 0) { Fail "not validly signed: $($invalid -join ', ')" }
$foreign = @($unsigned | Where-Object {
  $signature = Get-AuthenticodeSignature $_
  $signature.SignerCertificate.Thumbprint -ne $env:WINDOWS_SIGN_CERT_SHA1 -or -not $signature.TimeStamperCertificate
})
if ($foreign.Count -gt 0) { Fail "not signed with $env:WINDOWS_SIGN_CERT_SHA1 and a timestamp: $($foreign -join ', ')" }
Write-Output "sign: all $($paths.Count) file(s) validly signed"
