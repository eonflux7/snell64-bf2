# [CHOSEN, not recovered] The Certum SimplySign session of the release CI's Windows signing job (docs/ci-releases.md,
# Windows signing).
#
#   login   installs SimplySign Desktop (pinned version, SHA-256 and Asseco Data Systems signature), logs in with its
#           /autologin switch and a one-time code computed from CERTUM_OTPAUTH_URI, waits until the cloud certificate
#           appears in the user's store, and exports to GITHUB_ENV: WINDOWS_SIGNTOOL and WINDOWS_SIGN_CERT_SHA1 for
#           sign.ps1, SETUP_SIGNTOOL, SETUP_SIGN_CERT_SHA1 and SETUP_SIGN_TIMESTAMP for the setup's sign.ps1.
#   logout  ends the session (run it even when a step failed).
#   functions  only defines the URI and code functions, for a test that dot-sources this script.
#
# Environment: CERTUM_OTPAUTH_URI (secret), the otpauth://totp/... URI of the SimplySign activation QR code; its label
# carries the login ("Certum:<login>"), CERTUM_LOGIN overrides it. CERTUM_CERT_SHA1 (optional) is the thumbprint
# the certificate must have. Certum refuses a code from a period whose code was already used and may lock the
# account after repeated wrong codes, so every attempt waits for a new period and there are three at most.
param([Parameter(Mandatory = $true)][ValidateSet('login', 'logout', 'functions')][string]$Action)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$version = '9.4.5.95'
$sha256 = '6309e8b46f1f4e786288e777aef515a2c30d885165b71a21191e9941badebd6b'
$timestamp = 'http://time.certum.pl'

function Find-SimplySign {
  foreach ($root in $env:ProgramFiles, ${env:ProgramFiles(x86)}) {
    $exe = Get-ChildItem (Join-Path $root 'Certum') -Recurse -Filter SimplySignDesktop.exe -ErrorAction SilentlyContinue |
      Select-Object -First 1
    if ($exe) { return $exe.FullName }
  }
}

if ($Action -eq 'logout') {
  $exe = Find-SimplySign
  if ($exe -and (Get-Process SimplySignDesktop -ErrorAction SilentlyContinue)) {
    # /close asks the running instance to log out of the cloud and quit; the requesting process is not awaited
    Start-Process $exe -ArgumentList '/close' | Out-Null
    Wait-Process SimplySignDesktop -Timeout 20 -ErrorAction SilentlyContinue
    Get-Process SimplySignDesktop -ErrorAction SilentlyContinue | Stop-Process -Force
  }
  Write-Output 'simplysign: logged out'
  exit 0
}

# --- the one-time code (RFC 6238) from the activation URI ---
function Read-ActivationUri([string]$text) {
  $uri = [Uri]$text.Trim()
  if ($uri.Scheme -ne 'otpauth' -or $uri.Host -ne 'totp') { throw 'CERTUM_OTPAUTH_URI is not an otpauth://totp/ URI' }
  $query = @{}
  foreach ($pair in ($uri.Query.TrimStart('?') -split '&')) {
    if (-not $pair) { continue }
    $name, $value = $pair -split '=', 2
    $query[[Uri]::UnescapeDataString($name).ToLowerInvariant()] = [Uri]::UnescapeDataString("$value")
  }
  if (-not $query['secret']) { throw 'CERTUM_OTPAUTH_URI has no secret' }
  $label = [Uri]::UnescapeDataString($uri.AbsolutePath.TrimStart('/'))
  [pscustomobject]@{
    Secret = $query['secret']
    Algorithm = if ($query['algorithm']) { $query['algorithm'].ToUpperInvariant() } else { 'SHA1' }
    Digits = if ($query['digits']) { [int]$query['digits'] } else { 6 }
    Period = if ($query['period']) { [int]$query['period'] } else { 30 }
    Login = $(if ($label.Contains(':')) { ($label -split ':', 2)[1] } else { $label }).Trim()
  }
}

function ConvertFrom-Base32([string]$text) {
  $alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'
  $bytes = [System.Collections.Generic.List[byte]]::new()
  $buffer = 0; $bits = 0
  foreach ($character in $text.ToUpperInvariant().Replace(' ', '').TrimEnd('=').ToCharArray()) {
    $index = $alphabet.IndexOf($character)
    if ($index -lt 0) { throw 'the TOTP secret is not Base32' }
    $buffer = ($buffer -shl 5) -bor $index; $bits += 5
    if ($bits -ge 8) { $bits -= 8; $bytes.Add([byte](($buffer -shr $bits) -band 0xFF)); $buffer = $buffer -band ((1 -shl $bits) - 1) }
  }
  return ,$bytes.ToArray()
}

function Get-OneTimeCode([byte[]]$key, [long]$counter, [string]$algorithm, [int]$digits) {
  $hmac = switch ($algorithm) {
    'SHA1' { [System.Security.Cryptography.HMACSHA1]::new($key) }
    'SHA256' { [System.Security.Cryptography.HMACSHA256]::new($key) }
    'SHA512' { [System.Security.Cryptography.HMACSHA512]::new($key) }
    default { throw "unsupported TOTP algorithm $algorithm" }
  }
  $message = [BitConverter]::GetBytes($counter)
  if ([BitConverter]::IsLittleEndian) { [Array]::Reverse($message) }
  $hash = $hmac.ComputeHash($message)
  $offset = $hash[$hash.Length - 1] -band 0x0F
  $binary = (([long]$hash[$offset] -band 0x7F) -shl 24) -bor ([long]$hash[$offset + 1] -shl 16) -bor
    ([long]$hash[$offset + 2] -shl 8) -bor [long]$hash[$offset + 3]
  return ($binary % [long][Math]::Pow(10, $digits)).ToString("D$digits")
}
if ($Action -eq 'functions') { return }  # dot-sourced by a test: the definitions only

if (-not $env:CERTUM_OTPAUTH_URI) { throw 'the release environment lacks the secret CERTUM_OTPAUTH_URI (docs/ci-releases.md)' }
Write-Output "::add-mask::$($env:CERTUM_OTPAUTH_URI.Trim())"
$otp = Read-ActivationUri $env:CERTUM_OTPAUTH_URI
Write-Output "::add-mask::$($otp.Secret)"
$login = if ($env:CERTUM_LOGIN) { $env:CERTUM_LOGIN.Trim() } else { $otp.Login }
if (-not $login) { throw 'the activation URI names no login: set CERTUM_LOGIN' }
Write-Output "::add-mask::$login"
$key = ConvertFrom-Base32 $otp.Secret

# --- SimplySign Desktop and signtool ---
$exe = Find-SimplySign
if (-not $exe) {
  $msi = Join-Path $env:RUNNER_TEMP "SimplySignDesktop-$version.msi"
  Invoke-WebRequest "https://files.certum.eu/software/SimplySignDesktop/Windows/$version/SimplySignDesktop-$version-64-bit-en.msi" -OutFile $msi -UseBasicParsing
  if ((Get-FileHash $msi -Algorithm SHA256).Hash.ToLower() -ne $sha256) { throw "SimplySign Desktop $version does not have its pinned SHA-256" }
  $signature = Get-AuthenticodeSignature $msi
  if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Asseco Data Systems S\.A\.') {
    throw "SimplySign Desktop is not signed by Asseco Data Systems: $($signature.Status) $($signature.SignerCertificate.Subject)"
  }
  $install = Start-Process msiexec.exe -ArgumentList '/i', "`"$msi`"", '/qn', '/norestart', '/l*v', "`"$env:RUNNER_TEMP\simplysign-install.log`"" -Wait -PassThru
  if ($install.ExitCode -notin 0, 3010) { throw "installing SimplySign Desktop failed with exit code $($install.ExitCode)" }
  $exe = Find-SimplySign
  if (-not $exe) { throw 'SimplySignDesktop.exe is missing after the installation' }
}
# the SimplySign key provider is 64-bit: the x64 signtool of the newest Windows SDK
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\signtool.exe" -ErrorAction SilentlyContinue |
  Sort-Object { [version]$_.Directory.Parent.Name } | Select-Object -Last 1
if (-not $signtool) { throw 'no x64 signtool.exe of the Windows SDK' }

# --- the login ---
$expected = ("$env:CERTUM_CERT_SHA1" -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
function Find-Certificate {
  $certificates = @(Get-ChildItem Cert:\CurrentUser\My | Where-Object {
    $_.Issuer -match 'Certum' -and $_.NotAfter -gt (Get-Date) -and $_.EnhancedKeyUsageList.ObjectId -contains '1.3.6.1.5.5.7.3.3'
  })
  if ($expected) { return $certificates | Where-Object Thumbprint -eq $expected | Select-Object -First 1 }
  if ($certificates.Count -gt 1) { throw "several Certum code signing certificates ($($certificates.Thumbprint -join ', ')): set CERTUM_CERT_SHA1" }
  return $certificates | Select-Object -First 1
}
$certificate = $null
$used = -1L
for ($attempt = 1; $attempt -le 3 -and -not $certificate; ++$attempt) {
  # a period not used before, with at least 10 seconds left
  while ($true) {
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $counter = [long][Math]::Floor($now / $otp.Period)
    $left = $otp.Period - ($now % $otp.Period)
    if ($counter -gt $used -and $left -ge 10) { break }
    Start-Sleep -Seconds ($left + 1)
  }
  $used = $counter
  $code = Get-OneTimeCode $key $counter $otp.Algorithm $otp.Digits
  Write-Output "::add-mask::$code"
  Write-Output "simplysign: login attempt $attempt"
  # a refused login ends the process; an accepted one keeps it running and registers the certificate
  $process = Start-Process $exe -ArgumentList '/autologin', $login, $code -PassThru
  $deadline = (Get-Date).AddSeconds(90)
  while ((Get-Date) -lt $deadline -and -not $certificate) {
    $certificate = Find-Certificate
    if (-not $certificate -and $process.HasExited) { break }
    if (-not $certificate) { Start-Sleep -Seconds 2 }
  }
  if (-not $certificate -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
if (-not $certificate) {
  $logs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'SimplySignLog'
  Get-ChildItem $logs -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1 | Get-Content -Tail 40
  throw 'SimplySign login failed: no Certum code signing certificate appeared in the user''s store'
}
Write-Output "simplysign: $($certificate.Subject), $($certificate.Thumbprint), valid until $($certificate.NotAfter.ToString('yyyy-MM-dd'))"
if ($certificate.NotAfter -lt (Get-Date).AddDays(30)) {
  Write-Output "::warning title=Certum certificate::the code signing certificate expires on $($certificate.NotAfter.ToString('yyyy-MM-dd'))"
}
"WINDOWS_SIGNTOOL=$($signtool.FullName)", "WINDOWS_SIGN_CERT_SHA1=$($certificate.Thumbprint)",
  "SETUP_SIGNTOOL=$($signtool.FullName)", "SETUP_SIGN_CERT_SHA1=$($certificate.Thumbprint)", "SETUP_SIGN_TIMESTAMP=$timestamp" |
  Add-Content $env:GITHUB_ENV
