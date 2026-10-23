#Requires -Version 7.2
[CmdletBinding()]
param(
    [string] $ObsPath = "${env:ProgramFiles}/obs-studio/bin/64bit/obs64.exe",
    [string] $ObsTag = $env:OBS_DEBUG_SYMBOLS_TAG,
    [string] $CacheRoot = "$PSScriptRoot/../_deps"
)

$ErrorActionPreference = 'Stop'

function Read-BinaryRange([System.IO.BinaryReader] $Reader, [long] $Offset, [int] $Count) {
    if ($Offset -lt 0 -or $Count -lt 0 -or $Offset -gt $Reader.BaseStream.Length - $Count) {
        throw "Invalid binary range: offset $Offset, length $Count."
    }
    $Reader.BaseStream.Position = $Offset
    return , $Reader.ReadBytes($Count)
}

function Get-PeDebugIdentity([string] $Path) {
    $Reader = [System.IO.BinaryReader]::new([System.IO.File]::OpenRead($Path))
    try {
        $Header = Read-BinaryRange $Reader 0 64
        if ([BitConverter]::ToUInt16($Header, 0) -ne 0x5A4D) { throw "Not a PE file: $Path" }
        $PeOffset = [BitConverter]::ToUInt32($Header, 60)
        $Pe = Read-BinaryRange $Reader $PeOffset 24
        if ([BitConverter]::ToUInt32($Pe, 0) -ne 0x4550) { throw "Invalid PE signature: $Path" }
        $Architecture = switch ([BitConverter]::ToUInt16($Pe, 4)) {
            0x8664 { 'x64' }
            0xAA64 { 'arm64' }
            default { throw "Unsupported OBS architecture: $Path" }
        }
        $SectionCount = [BitConverter]::ToUInt16($Pe, 6)
        $OptionalSize = [BitConverter]::ToUInt16($Pe, 20)
        $Optional = Read-BinaryRange $Reader ($PeOffset + 24) $OptionalSize
        if ($OptionalSize -lt 168 -or [BitConverter]::ToUInt16($Optional, 0) -ne 0x20B -or
            [BitConverter]::ToUInt32($Optional, 108) -lt 7) { throw "Missing PE debug directory: $Path" }
        $DebugRva = [BitConverter]::ToUInt32($Optional, 160)
        $DebugSize = [BitConverter]::ToUInt32($Optional, 164)
        $DebugOffset = $null
        for ($SectionIndex = 0; $SectionIndex -lt $SectionCount; $SectionIndex++) {
            $Section = Read-BinaryRange $Reader ($PeOffset + 24 + $OptionalSize + 40 * $SectionIndex) 40
            $VirtualAddress = [BitConverter]::ToUInt32($Section, 12)
            $RawSize = [BitConverter]::ToUInt32($Section, 16)
            if ($DebugRva -ge $VirtualAddress -and [long]$DebugRva + $DebugSize -le [long]$VirtualAddress + $RawSize) {
                $DebugOffset = [long][BitConverter]::ToUInt32($Section, 20) + $DebugRva - $VirtualAddress
                break
            }
        }
        if ($null -eq $DebugOffset -or $DebugSize -eq 0 -or $DebugSize % 28 -ne 0) {
            throw "Invalid PE debug directory: $Path"
        }
        for ($EntryOffset = 0; $EntryOffset -lt $DebugSize; $EntryOffset += 28) {
            $Entry = Read-BinaryRange $Reader ($DebugOffset + $EntryOffset) 28
            if ([BitConverter]::ToUInt32($Entry, 12) -ne 2) { continue }
            $Size = [BitConverter]::ToUInt32($Entry, 16)
            if ($Size -lt 25 -or $Size -gt 65536) { throw "Invalid CodeView record: $Path" }
            $CodeView = Read-BinaryRange $Reader ([BitConverter]::ToUInt32($Entry, 24)) $Size
            if ([Text.Encoding]::ASCII.GetString($CodeView, 0, 4) -ne 'RSDS') { continue }
            $PdbPath = [Text.Encoding]::UTF8.GetString($CodeView, 24, $Size - 24).Split([char]0)[0]
            return [pscustomobject]@{
                Architecture = $Architecture
                Guid         = [guid]::new([byte[]]$CodeView[4..19]).ToString()
                Age          = [BitConverter]::ToUInt32($CodeView, 20)
                PdbName      = ($PdbPath -split '[\\/]')[-1]
            }
        }
        throw "No RSDS debug identity: $Path"
    }
    finally { $Reader.Dispose() }
}

function Invoke-GitHubJson([string] $Endpoint, [switch] $AllowMissing) {
    $Output = & gh api $Endpoint 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        if ($AllowMissing -and $Output -match 'HTTP 404') { return $null }
        throw "GitHub request failed ($Endpoint): $Output"
    }
    return ConvertFrom-Json -AsHashtable $Output
}

function Get-ObsVersion([string] $VersionText) {
    if ($VersionText -notmatch '^(\d+\.\d+\.\d+)(?:\.\d+)?(?<suffix>-(?:rc|beta)\d+)?$') {
        throw "Unrecognized OBS version '$VersionText'; supply -ObsTag for a custom build."
    }
    return "$($Matches[1])$($Matches.suffix)"
}

function Get-ObsCandidateTags([string] $Version, [string] $Override) {
    if ($Override) {
        if ($Override -notmatch '^\d+\.\d+\.\d+(?:-(?:rc|beta)\d+)?$') { throw "Invalid OBS tag '$Override'." }
        return $Override
    }
    if ($Version -match '-(?:rc|beta)\d+$') { return $Version }
    $References = Invoke-GitHubJson "repos/obsproject/obs-studio/git/matching-refs/tags/$Version"
    $Pattern = '^' + [regex]::Escape($Version) + '(?:-(?:rc|beta)\d+)?$'
    $Tags = @($References | ForEach-Object { $_.ref -replace '^refs/tags/', '' } | Where-Object { $_ -match $Pattern })
    if ($Tags.Count -eq 0) { throw "No official OBS tags match $Version. Supply -ObsTag for the matching release." }
    return $Tags | Sort-Object @{ Expression = { $_ -eq $Version }; Descending = $true },
    @{ Expression = { $_ -match '-rc' }; Descending = $true },
    @{ Expression = { if ($_ -match '(\d+)$') { [int]$Matches[1] } else { 0 } }; Descending = $true }
}

function Get-ObsQtDependency([string] $Tag, [string] $Architecture) {
    $Content = Invoke-GitHubJson "repos/obsproject/obs-studio/contents/CMakePresets.json?ref=$Tag" -AllowMissing
    $Dependency = $null
    if ($Content) {
        $Presets = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Content.content)) | ConvertFrom-Json -AsHashtable
        foreach ($Preset in $Presets.configurePresets) {
            if ($Preset.vendor -and $Preset.vendor.ContainsKey('obsproject.com/obs-studio')) {
                $Dependency = $Preset.vendor['obsproject.com/obs-studio'].dependencies.qt6
                if ($Dependency) { break }
            }
        }
    }
    if (!$Dependency) {
        $Content = Invoke-GitHubJson "repos/obsproject/obs-studio/contents/buildspec.json?ref=$Tag" -AllowMissing
        if ($Content) {
            $BuildSpec = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Content.content)) | ConvertFrom-Json -AsHashtable
            $Dependency = $BuildSpec.dependencies.qt6
        }
    }
    if (!$Dependency) { throw "OBS $Tag has no supported Qt dependency metadata." }
    $Platform = "windows-$Architecture"
    $PlatformKey = @($Dependency.debugSymbols.Keys | Where-Object { $_ -ieq $Platform })[0]
    $Checksum = if ($PlatformKey) { $Dependency.debugSymbols[$PlatformKey] } else { '' }
    if ($Checksum -notmatch '^[a-fA-F0-9]{64}$' -or $Dependency.version -notmatch '^\d{4}-\d{2}-\d{2}$' -or
        $Dependency.baseUrl -ne 'https://github.com/obsproject/obs-deps/releases/download') {
        throw "OBS $Tag has no valid published Qt PDB descriptor for $Architecture."
    }
    $Revision = if ($Dependency.revision) { $Dependency.revision[$PlatformKey] } else { $null }
    if ($Revision -and "$Revision" -notmatch '^\d+$') { throw "Invalid Qt archive revision in OBS $Tag." }
    $Suffix = if ($Revision) { "-v$Revision" } else { '' }
    return [pscustomobject]@{
        Tag       = $Tag
        Version   = $Dependency.version
        Checksum  = $Checksum.ToLowerInvariant()
        Asset     = "windows-deps-qt6-$($Dependency.version)-$Architecture$Suffix-PDBs.zip"
        Directory = "obs-deps-qt6-$($Dependency.version)-$Architecture$Suffix-PDBs"
    }
}

function Test-QtSymbols([string] $Directory, [object[]] $Modules) {
    foreach ($Module in $Modules) {
        $Path = Join-Path $Directory $Module.PdbName
        if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
        $Identity = Get-PdbDebugIdentity $Path
        if ($Identity.Guid -ne $Module.Guid -or $Identity.Age -ne $Module.Age) { return $false }
    }
    return $true
}

function Expand-VerifiedSymbols([string] $Archive, [string] $Destination, [string] $Checksum) {
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ine $Checksum) {
        throw "Qt PDB archive checksum mismatch: $Archive"
    }
    $Zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $Root = [IO.Path]::GetFullPath($Destination) + [IO.Path]::DirectorySeparatorChar
        foreach ($Entry in $Zip.Entries) {
            $Name = $Entry.FullName.Replace('\', '/')
            $Target = [IO.Path]::GetFullPath((Join-Path $Destination $Name))
            if (!$Target.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Unsafe path in Qt PDB archive: $Name"
            }
        }
    }
    finally { $Zip.Dispose() }
    Expand-Archive -LiteralPath $Archive -DestinationPath $Destination
}

function Get-QtSymbolPackage([object] $Dependency, [string] $Architecture, [string] $Root) {
    $ArchitectureRoot = Join-Path $Root $Architecture
    New-Item -ItemType Directory -Force $ArchitectureRoot | Out-Null
    $Destination = Join-Path $ArchitectureRoot $Dependency.Directory
    $Archive = Join-Path $ArchitectureRoot $Dependency.Asset
    $LegacyArchive = Join-Path $Destination $Dependency.Asset
    if (!(Test-Path -LiteralPath $Archive) -and (Test-Path -LiteralPath $LegacyArchive)) { $Archive = $LegacyArchive }
    if (!(Test-Path -LiteralPath $Archive)) {
        Get-Command gh -ErrorAction Stop | Out-Null
        $Download = Join-Path $ArchitectureRoot "download-$([guid]::NewGuid())"
        New-Item -ItemType Directory $Download | Out-Null
        try {
            Write-Host "Downloading $($Dependency.Asset)"
            & gh release download $Dependency.Version --repo obsproject/obs-deps --pattern $Dependency.Asset --dir $Download
            if ($LASTEXITCODE -ne 0) { throw "Qt PDB download failed. Retry when GitHub is reachable." }
            $DownloadedArchive = Join-Path $Download $Dependency.Asset
            if ((Get-FileHash $DownloadedArchive -Algorithm SHA256).Hash -ine $Dependency.Checksum) {
                throw "Downloaded Qt PDB archive checksum mismatch."
            }
            Move-Item -LiteralPath $DownloadedArchive -Destination $Archive
        }
        finally { Remove-Item -LiteralPath $Download -Recurse -Force }
    }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ine $Dependency.Checksum) {
        throw "Cached Qt PDB archive checksum mismatch: $Archive. Remove this archive and retry."
    }
    $Marker = Join-Path $Destination 'archive.sha256'
    if ((Test-Path -LiteralPath $Marker) -and (Get-Content $Marker -Raw).Trim() -eq $Dependency.Checksum) {
        return Join-Path $Destination 'bin'
    }
    $Staging = "$Destination.staging-$([guid]::NewGuid())"
    try {
        Expand-VerifiedSymbols $Archive $Staging $Dependency.Checksum
        if (!(Test-Path "$Staging/bin/Qt6Core.pdb")) { throw 'Qt archive is missing bin/Qt6Core.pdb.' }
        Set-Content "$Staging/archive.sha256" $Dependency.Checksum
        if (Test-Path -LiteralPath $Destination) {
            if ($Archive -eq $LegacyArchive) { Copy-Item -LiteralPath $Archive -Destination $Staging }
            Remove-Item -LiteralPath $Destination -Recurse -Force
        }
        Move-Item -LiteralPath $Staging -Destination $Destination
    }
    finally { if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force } }
    return Join-Path $Destination 'bin'
}

function Get-ObsSymbolPackage([string] $Tag, [string] $Architecture, [string] $Root, [object[]] $Modules) {
    $Release = Invoke-GitHubJson "repos/obsproject/obs-studio/releases/tags/$Tag" -AllowMissing
    if (!$Release) { return $null }
    $AssetName = "OBS-Studio-$Tag-Windows-$Architecture-PDBs.zip"
    $Asset = @($Release.assets | Where-Object { $_.name -ceq $AssetName })
    if ($Asset.Count -ne 1) { return $null }
    if ($Asset[0].digest -notmatch '^sha256:([a-fA-F0-9]{64})$') {
        throw "OBS PDB archive has no valid SHA256 digest: $AssetName"
    }
    $Checksum = $Matches[1].ToLowerInvariant()
    $Destination = Join-Path $Root "obs-pdbs-$Tag-$Architecture"
    if (Test-QtSymbols "$Destination/bin" $Modules) { return "$Destination/bin" }
    $Staging = "$Destination.staging-$([guid]::NewGuid())"
    New-Item -ItemType Directory -Force $Staging | Out-Null
    try {
        Write-Host "Downloading $AssetName"
        & gh release download $Tag --repo obsproject/obs-studio --pattern $AssetName --dir $Staging
        if ($LASTEXITCODE -ne 0) { throw 'OBS PDB download failed. Retry when GitHub is reachable.' }
        Expand-VerifiedSymbols (Join-Path $Staging $AssetName) "$Staging/extracted" $Checksum
        New-Item -ItemType Directory "$Staging/bin" | Out-Null
        foreach ($Module in $Modules) {
            $Candidates = @(Get-ChildItem "$Staging/extracted" -Recurse -File -Filter $Module.PdbName)
            $Matched = $false
            foreach ($Candidate in $Candidates) {
                $Identity = Get-PdbDebugIdentity $Candidate.FullName
                if ($Identity.Guid -eq $Module.Guid -and $Identity.Age -eq $Module.Age) {
                    Copy-Item -LiteralPath $Candidate.FullName -Destination "$Staging/bin"
                    $Matched = $true
                    break
                }
            }
            if (!$Matched) {
                Write-Host "OBS $Tag PDB does not match $($Module.PdbName); checking the next candidate."
                return $null
            }
        }
        Remove-Item -LiteralPath "$Staging/extracted", (Join-Path $Staging $AssetName) -Recurse -Force
        if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
        Move-Item -LiteralPath $Staging -Destination $Destination
        return "$Destination/bin"
    }
    finally { if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force } }
}

function Publish-QtSymbols([string] $Source, [string] $Destination, [object[]] $Modules, [object] $Manifest,
    [string] $ObsSource, [object[]] $ObsModules = @()) {
    $Staging = "$Destination.staging-$([guid]::NewGuid())"
    $Backup = "$Destination.backup-$([guid]::NewGuid())"
    try {
        New-Item -ItemType Directory -Force "$Staging/bin" | Out-Null
        foreach ($Module in $Modules) { Copy-Item -LiteralPath (Join-Path $Source $Module.PdbName) -Destination "$Staging/bin" }
        if (!(Test-QtSymbols "$Staging/bin" $Modules)) { throw 'Staged Qt symbols do not match the installed DLLs.' }
        foreach ($Module in $ObsModules) { Copy-Item -LiteralPath (Join-Path $ObsSource $Module.PdbName) -Destination "$Staging/bin" }
        if (!(Test-QtSymbols "$Staging/bin" $ObsModules)) { throw 'Staged OBS symbols do not match the installed binaries.' }
        $Manifest | ConvertTo-Json -Depth 8 | Set-Content "$Staging/manifest.json" -Encoding utf8
        if (Test-Path -LiteralPath $Destination) { Move-Item -LiteralPath $Destination -Destination $Backup }
        try { Move-Item -LiteralPath $Staging -Destination $Destination }
        catch {
            if (Test-Path -LiteralPath $Backup) { Move-Item -LiteralPath $Backup -Destination $Destination }
            throw
        }
    }
    finally {
        if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
        if ((Test-Path -LiteralPath $Backup) -and (Test-Path -LiteralPath $Destination)) {
            Remove-Item -LiteralPath $Backup -Recurse -Force
        }
    }
}

function Initialize-ObsQtSymbols([string] $Executable, [string] $Tag, [string] $Root) {
    $Executable = (Resolve-Path -LiteralPath $Executable).Path
    $VersionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
    $VersionText = if ($VersionInfo.ProductVersion) { $VersionInfo.ProductVersion } else { $VersionInfo.FileVersion }
    $Version = if ($Tag) { $Tag } else { Get-ObsVersion $VersionText }
    $Architecture = (Get-PeDebugIdentity $Executable).Architecture
    $Runtime = Split-Path $Executable -Parent
    foreach ($Name in 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll') {
        if (!(Test-Path (Join-Path $Runtime $Name))) { throw "OBS runtime is missing $Name." }
    }
    $Modules = @(Get-ChildItem -LiteralPath $Runtime -Filter 'Qt6*.dll' | ForEach-Object {
            $Identity = Get-PeDebugIdentity $_.FullName
            if ($Identity.Architecture -ne $Architecture) { throw "Qt runtime architecture mismatch: $($_.Name)" }
            if ($Identity.PdbName -notmatch '^Qt6[A-Za-z0-9_]+\.pdb$') { throw "Unexpected Qt PDB name: $($Identity.PdbName)" }
            $Identity
        })
    $ObsModules = @(foreach ($Name in 'obs64.exe', 'obs.dll', 'obs-frontend-api.dll') {
            $Identity = Get-PeDebugIdentity (Join-Path $Runtime $Name)
            if ($Identity.Architecture -ne $Architecture -or $Identity.PdbName -notmatch '^[A-Za-z0-9_.-]+\.pdb$') {
                throw "Invalid OBS debug identity: $Name"
            }
            $Identity
        })
    $Root = [IO.Path]::GetFullPath($Root)
    New-Item -ItemType Directory -Force $Root | Out-Null
    $Destination = Join-Path $Root 'obs-debug-symbols'
    $Lock = $null
    try {
        try { $Lock = [IO.File]::Open("$Root/obs-debug-symbols.lock", 'OpenOrCreate', 'ReadWrite', 'None') }
        catch { throw 'Another OBS symbol preparation is running. Retry after it finishes.' }
        Write-Host "Installed OBS: $VersionText ($Architecture), $Executable"
        $ManifestPath = Join-Path $Destination 'manifest.json'
        if (Test-Path -LiteralPath $ManifestPath) {
            $Manifest = Get-Content $ManifestPath -Raw | ConvertFrom-Json
            if ($Manifest.ObsPath -eq $Executable -and $Manifest.ObsVersion -eq $VersionText -and
                (!$Tag -or $Manifest.ObsTag -eq $Tag) -and (Test-QtSymbols "$Destination/bin" ($Modules + $ObsModules))) {
                Write-Host "OBS and Qt symbols already verified: $($Manifest.ObsTag) (offline cache hit)"
                return
            }
        }
        Get-Command gh -ErrorAction Stop | Out-Null
        $QtPackages = @{}
        foreach ($Candidate in @(Get-ObsCandidateTags $Version $Tag)) {
            $Dependency = Get-ObsQtDependency $Candidate $Architecture
            Write-Host "Checking OBS $Candidate / Qt deps $($Dependency.Version)"
            if (!$QtPackages.ContainsKey($Dependency.Checksum)) {
                $QtPackages[$Dependency.Checksum] = Get-QtSymbolPackage $Dependency $Architecture $Root
            }
            $Symbols = $QtPackages[$Dependency.Checksum]
            if (!(Test-QtSymbols $Symbols $Modules)) {
                Write-Host "Qt PDB identities do not match OBS $Candidate; checking the next candidate."
                continue
            }
            $ObsSymbols = Get-ObsSymbolPackage $Candidate $Architecture $Root $ObsModules
            if (!$ObsSymbols) { continue }
            $Manifest = [ordered]@{
                ObsPath = $Executable; ObsVersion = $VersionText; Architecture = $Architecture
                ObsTag = $Candidate; Asset = $Dependency.Asset; Checksum = $Dependency.Checksum; Modules = ($Modules + $ObsModules)
            }
            Publish-QtSymbols $Symbols $Destination $Modules $Manifest $ObsSymbols $ObsModules
            Write-Host "Verified $($Modules.Count) Qt and $($ObsModules.Count) OBS GUID/age pairs. Symbols ready: $Destination/bin"
            return
        }
        throw 'No official OBS/Qt PDB archive combination matches the installed binaries. Supply -ObsTag / OBS_DEBUG_SYMBOLS_TAG for the exact build; custom builds require their own PDBs.'
    }
    finally { if ($Lock) { $Lock.Dispose() } }
}

function Get-PdbDebugIdentity([string] $Path) {
    $Reader = [System.IO.BinaryReader]::new([System.IO.File]::OpenRead($Path))
    try {
        $Header = Read-BinaryRange $Reader 0 56
        $Signature = "Microsoft C/C++ MSF 7.00`r`n$([char]0x1A)DS$([char]0)$([char]0)$([char]0)"
        if ([Text.Encoding]::ASCII.GetString($Header, 0, 32) -ne $Signature) { throw "Not an MSF7 PDB: $Path" }
        $BlockSize = [BitConverter]::ToUInt32($Header, 32)
        $BlockCount = [BitConverter]::ToUInt32($Header, 40)
        $DirectorySize = [BitConverter]::ToUInt32($Header, 44)
        $BlockMap = [BitConverter]::ToUInt32($Header, 52)
        if ($BlockSize -notin 512, 1024, 2048, 4096 -or $BlockCount -eq 0 -or
            [long]$BlockCount * $BlockSize -gt $Reader.BaseStream.Length -or
            $DirectorySize -lt 12 -or $DirectorySize -gt 16777216 -or $BlockMap -ge $BlockCount) {
            throw "Invalid MSF7 header: $Path"
        }
        $DirectoryBlocks = [int][Math]::Ceiling($DirectorySize / [double]$BlockSize)
        if ($DirectoryBlocks * 4 -gt $BlockSize) { throw "Unsupported MSF7 directory map: $Path" }
        $Map = Read-BinaryRange $Reader ([long]$BlockMap * $BlockSize) ($DirectoryBlocks * 4)
        $Directory = [byte[]]::new($DirectorySize)
        for ($BlockIndex = 0; $BlockIndex -lt $DirectoryBlocks; $BlockIndex++) {
            $Block = [BitConverter]::ToUInt32($Map, $BlockIndex * 4)
            if ($Block -ge $BlockCount) { throw "Invalid MSF7 directory block: $Path" }
            $Count = [int][Math]::Min($BlockSize, $DirectorySize - $BlockIndex * $BlockSize)
            $Bytes = Read-BinaryRange $Reader ([long]$Block * $BlockSize) $Count
            [Array]::Copy($Bytes, 0, $Directory, $BlockIndex * $BlockSize, $Count)
        }
        $StreamCount = [BitConverter]::ToUInt32($Directory, 0)
        if ($StreamCount -lt 2 -or 4 + [long]$StreamCount * 4 -gt $DirectorySize) {
            throw "Invalid MSF7 stream count: $Path"
        }
        $InfoSize = [BitConverter]::ToUInt32($Directory, 8)
        if ($InfoSize -lt 28 -or $InfoSize -eq [uint32]::MaxValue) { throw "Missing PDB info stream: $Path" }
        $FirstSize = [BitConverter]::ToUInt32($Directory, 4)
        $FirstBlocks = if ($FirstSize -eq [uint32]::MaxValue) { 0 } else { [Math]::Ceiling($FirstSize / [double]$BlockSize) }
        $InfoMapOffset = 4 + [long]$StreamCount * 4 + $FirstBlocks * 4
        $InfoBlocks = [Math]::Ceiling($InfoSize / [double]$BlockSize)
        if ($InfoMapOffset + $InfoBlocks * 4 -gt $DirectorySize) { throw "Invalid PDB info map: $Path" }
        $InfoBlock = [BitConverter]::ToUInt32($Directory, [int]$InfoMapOffset)
        if ($InfoBlock -ge $BlockCount) { throw "Invalid PDB info block: $Path" }
        $Info = Read-BinaryRange $Reader ([long]$InfoBlock * $BlockSize) 28
        if ([BitConverter]::ToUInt32($Info, 0) -lt 20000404) { throw "Unsupported PDB info version: $Path" }
        return [pscustomobject]@{
            Guid = [guid]::new([byte[]]$Info[12..27]).ToString()
            Age  = [BitConverter]::ToUInt32($Info, 8)
        }
    }
    finally { $Reader.Dispose() }
}

if ($MyInvocation.InvocationName -ne '.') {
    Initialize-ObsQtSymbols $ObsPath $ObsTag $CacheRoot
}