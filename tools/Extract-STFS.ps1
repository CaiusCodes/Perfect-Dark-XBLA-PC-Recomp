# SPDX-License-Identifier: GPL-3.0-only
#
# Perfect Dark XBLA PC Recomp - Xbox 360 STFS (LIVE/PIRS/CON) package extractor.
# The STFS block-offset and hash-table calculations are derived from Velocity
# (https://github.com/hetelek/Velocity), Copyright (C) the Velocity authors,
# which is licensed under the GNU General Public License version 3. This script
# is therefore licensed under the GNU GPL version 3 as well (see
# packaging/licenses/LICENSE-Extract-STFS.txt); it is a developer tool and is
# not part of the release.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

<#
.SYNOPSIS
    Extracts files from Xbox 360 STFS (LIVE/PIRS/CON) containers.
.DESCRIPTION
    Implements the STFS file system as documented in the Free60 wiki and the
    Velocity project (https://github.com/hetelek/Velocity).
    Supports LIVE, PIRS, and CON package types with both Female (blockSep=1)
    and Male (blockSep=0) hash table layouts.
.PARAMETER Path
    Path to the STFS container file.
.PARAMETER OutputDir
    Directory to extract files to.
.PARAMETER ListOnly
    If specified, lists files without extracting.
.EXAMPLE
    .\Extract-STFS.ps1 -Path "DLC_Package" -OutputDir "C:\Extracted"
.EXAMPLE
    .\Extract-STFS.ps1 -Path "DLC_Package" -ListOnly
#>
param(
    [Parameter(Mandatory)]
    [string]$Path,
    [string]$OutputDir,
    [switch]$ListOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Relative paths are relative to the PowerShell location. The .NET file calls
# below would resolve them against the process's own working folder instead,
# which is often somewhere else, so both are made absolute first.
$Path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
if ($OutputDir) {
    $OutputDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
}

if (-not $ListOnly -and -not $OutputDir) {
    throw "OutputDir is required unless -ListOnly is specified."
}

#region Helper functions
function Read-Int24BE([byte[]]$b) {
    return ([int]$b[0] -shl 16) -bor ([int]$b[1] -shl 8) -bor [int]$b[2]
}

function Read-Int32BE([byte[]]$b) {
    return ([int]$b[0] -shl 24) -bor ([int]$b[1] -shl 16) -bor ([int]$b[2] -shl 8) -bor [int]$b[3]
}

function Read-Int24LE([byte[]]$b) {
    return [int]$b[0] -bor ([int]$b[1] -shl 8) -bor ([int]$b[2] -shl 16)
}

function Read-Int16LE([byte[]]$b) {
    return [int]$b[0] -bor ([int]$b[1] -shl 8)
}
#endregion

#region Block offset calculation (Velocity algorithm)
# These are set after reading the header
$script:Shift = 0
$script:FirstHashTableAddress = 0

function ComputeBackingDataBlockNumber([int]$blockNum) {
    # Velocity: StfsPackage::ComputeBackingDataBlockNumber
    $s = $script:Shift
    $toReturn = ([int][math]::Floor(($blockNum + 0xAA) / 0xAA) -shl $s) + $blockNum
    if ($blockNum -lt 0xAA) {
        return $toReturn
    } elseif ($blockNum -lt 0x70E4) {
        return $toReturn + ([int][math]::Floor(($blockNum + 0x70E4) / 0x70E4) -shl $s)
    } else {
        return (1 -shl $s) + ($toReturn + ([int][math]::Floor(($blockNum + 0x70E4) / 0x70E4) -shl $s))
    }
}

function BlockToAddress([int]$blockNum) {
    return [long]((ComputeBackingDataBlockNumber $blockNum) -shl 0xC) + [long]$script:FirstHashTableAddress
}

function ComputeLevel0BackingHashBlockNumber([int]$blockNum) {
    # Velocity: StfsPackage::ComputeLevel0BackingHashBlockNumber
    $s = $script:Shift
    if ($blockNum -lt 0xAA) { return 0 }

    if ($s -eq 0) {
        $step0 = 0xAB
    } else {
        $step0 = 0xAC
    }

    $num = [int][math]::Floor($blockNum / 0xAA) * $step0
    $num += ([int][math]::Floor($blockNum / 0x70E4) + 1) -shl $s

    if ([int][math]::Floor($blockNum / 0x70E4) -eq 0) {
        return $num
    }
    return $num + (1 -shl $s)
}

function Get-NextBlock {
    param(
        [System.IO.BinaryReader]$Reader,
        [int]$CurrentBlock
    )
    $hashBlockNum = ComputeLevel0BackingHashBlockNumber $CurrentBlock
    $hashAddr = [long]($hashBlockNum -shl 0xC) + [long]$script:FirstHashTableAddress
    $recordIndex = $CurrentBlock % 0xAA
    $recordOffset = $hashAddr + ($recordIndex * 0x18)

    $Reader.BaseStream.Position = $recordOffset + 0x14
    $null = $Reader.ReadByte()  # status
    $nextBlockBytes = $Reader.ReadBytes(3)
    $nextBlock = Read-Int24BE $nextBlockBytes

    if ($nextBlock -ge 0xFFFFFF -or $nextBlock -eq 0xFFFFFE) {
        return -1
    }
    return $nextBlock
}
#endregion

#region Main
$stfsFile = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read)
$reader = [System.IO.BinaryReader]::new($stfsFile)

try {
    # Read magic
    $magic = [System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
    if ($magic -notin @('CON ', 'LIVE', 'PIRS')) {
        throw "Not an STFS file (magic: $magic)"
    }
    Write-Host "Package type: $($magic.Trim())"

    # Read headerSize at 0x340 (4 bytes, Big Endian)
    $reader.BaseStream.Position = 0x340
    $headerSizeBytes = $reader.ReadBytes(4)
    $headerSize = Read-Int32BE $headerSizeBytes
    $script:FirstHashTableAddress = ($headerSize + 0xFFF) -band [int]0xFFFFF000
    Write-Host "Header size: 0x$($headerSize.ToString('X')), first hash table: 0x$($script:FirstHashTableAddress.ToString('X'))"

    # Read volume descriptor at 0x379
    $reader.BaseStream.Position = 0x379
    $volDescSize = $reader.ReadByte()
    $null = $reader.ReadByte()  # reserved
    $blockSeperation = $reader.ReadByte()

    # Velocity: packageSex = (~blockSeperation) & 1
    # Female (blockSep=1) → shift=0, Male (blockSep=0) → shift=1
    $script:Shift = (((-bnot $blockSeperation) -band 1) -bxor 1)
    # Actually: Female=shift 0, Male=shift 1. (~1)&1=0=Female, (~0)&1=1=Male
    $script:Shift = [int]((-bnot [int]$blockSeperation) -band 1) -bxor 1
    # Simpler: if blockSep=1 then shift=0, if blockSep=0 then shift=1
    if ($blockSeperation -band 1) { $script:Shift = 0 } else { $script:Shift = 1 }

    $fileTableBlockCountBytes = $reader.ReadBytes(2)
    $fileTableBlockCount = Read-Int16LE $fileTableBlockCountBytes

    $fileTableBlockNumBytes = $reader.ReadBytes(3)
    $fileTableBlockNum = Read-Int24LE $fileTableBlockNumBytes

    Write-Host "Block separation: $blockSeperation (shift=$($script:Shift))"
    Write-Host "File table: block $fileTableBlockNum, $fileTableBlockCount block(s)"

    # Read display name at 0x0411 (unicode, big-endian)
    $reader.BaseStream.Position = 0x0411
    $displayNameBytes = $reader.ReadBytes(128)
    $displayName = [System.Text.Encoding]::BigEndianUnicode.GetString($displayNameBytes).TrimEnd("`0")
    Write-Host "Display name: $displayName"

    # Read file table
    $fileTableOffset = BlockToAddress $fileTableBlockNum
    Write-Host "File table offset: 0x$($fileTableOffset.ToString('X'))"

    $entries = [System.Collections.ArrayList]::new()
    $directories = @{}
    $entryIndex = 0
    $currentBlock = $fileTableBlockNum
    $blocksRead = 0

    while ($blocksRead -lt $fileTableBlockCount) {
        $blockOffset = BlockToAddress $currentBlock

        for ($i = 0; $i -lt 64; $i++) {
            $entryOffset = $blockOffset + ($i * 0x40)
            if ($entryOffset + 0x40 -gt $stfsFile.Length) { break }
            $reader.BaseStream.Position = $entryOffset
            $entryData = $reader.ReadBytes(0x40)

            $nameLen = $entryData[0x28] -band 0x3F
            if ($nameLen -eq 0) { continue }

            # Verify the name bytes are printable ASCII
            $validName = $true
            for ($k = 0; $k -lt $nameLen; $k++) {
                if ($entryData[$k] -lt 0x20 -or $entryData[$k] -gt 0x7E) {
                    $validName = $false; break
                }
            }
            if (-not $validName) { continue }

            $isDirectory = ($entryData[0x28] -band 0x80) -ne 0
            $isConsecutive = ($entryData[0x28] -band 0x40) -ne 0
            $fileName = [System.Text.Encoding]::ASCII.GetString($entryData, 0, $nameLen)

            $numBlocks = Read-Int24LE @($entryData[0x29], $entryData[0x2A], $entryData[0x2B])
            $firstBlock = Read-Int24LE @($entryData[0x2F], $entryData[0x30], $entryData[0x31])
            $pathIndicator = ([int]$entryData[0x32] -shl 8) -bor [int]$entryData[0x33]
            if ($pathIndicator -eq 0xFFFF) { $pathIndicator = -1 }
            $fileSize = Read-Int32BE @($entryData[0x34], $entryData[0x35], $entryData[0x36], $entryData[0x37])

            $entry = [PSCustomObject]@{
                Index         = $entryIndex
                Name          = $fileName
                IsDirectory   = $isDirectory
                IsConsecutive = $isConsecutive
                NumBlocks     = $numBlocks
                FirstBlock    = $firstBlock
                PathIndicator = $pathIndicator
                FileSize      = $fileSize
            }
            [void]$entries.Add($entry)
            if ($isDirectory) { $directories[$entryIndex] = $entry }
            $entryIndex++
        }

        $blocksRead++
        if ($blocksRead -lt $fileTableBlockCount) {
            $nextBlock = Get-NextBlock -Reader $reader -CurrentBlock $currentBlock
            if ($nextBlock -eq -1) { break }
            $currentBlock = $nextBlock
        }
    }

    Write-Host "`nFound $($entries.Count) entries:"

    if ($ListOnly) {
        foreach ($entry in $entries) {
            $pathParts = @($entry.Name)
            $pi = $entry.PathIndicator
            while ($pi -ne -1 -and $directories.ContainsKey($pi)) {
                $pathParts = @($directories[$pi].Name) + $pathParts
                $pi = $directories[$pi].PathIndicator
            }
            $rel = $pathParts -join '\'
            if ($entry.IsDirectory) {
                Write-Host "  DIR:  $rel"
            } else {
                Write-Host "  FILE: $rel ($($entry.FileSize) bytes)"
            }
        }
    } else {
        if (-not (Test-Path $OutputDir)) {
            New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
        }

        foreach ($entry in $entries) {
            $pathParts = @($entry.Name)
            $pi = $entry.PathIndicator
            while ($pi -ne -1 -and $directories.ContainsKey($pi)) {
                $pathParts = @($directories[$pi].Name) + $pathParts
                $pi = $directories[$pi].PathIndicator
            }
            $relativePath = $pathParts -join '\'

            if ($entry.IsDirectory) {
                $dirPath = Join-Path $OutputDir $relativePath
                if (-not (Test-Path $dirPath)) {
                    New-Item -ItemType Directory -Path $dirPath -Force | Out-Null
                }
                Write-Host "  DIR:  $relativePath"
            } else {
                $filePath = Join-Path $OutputDir $relativePath
                $fileDir = Split-Path $filePath -Parent
                if (-not (Test-Path $fileDir)) {
                    New-Item -ItemType Directory -Path $fileDir -Force | Out-Null
                }

                Write-Host "  FILE: $relativePath ($($entry.FileSize) bytes)"

                $outStream = [System.IO.File]::Create($filePath)
                try {
                    $remaining = $entry.FileSize
                    $currentBlock = $entry.FirstBlock
                    $blocksExtracted = 0

                    while ($remaining -gt 0 -and $blocksExtracted -lt $entry.NumBlocks) {
                        $blockOffset = BlockToAddress $currentBlock

                        if ($blockOffset + 0x1000 -gt $stfsFile.Length + 0x1000) {
                            Write-Warning "Block $currentBlock offset 0x$($blockOffset.ToString('X')) exceeds file size for $($entry.Name)"
                            break
                        }

                        $reader.BaseStream.Position = $blockOffset
                        $toRead = [int][math]::Min($remaining, 0x1000)
                        $data = $reader.ReadBytes($toRead)
                        $outStream.Write($data, 0, $data.Length)
                        $remaining -= $data.Length
                        $blocksExtracted++

                        if ($remaining -gt 0) {
                            if ($entry.IsConsecutive) {
                                $currentBlock++
                            } else {
                                $nextBlock = Get-NextBlock -Reader $reader -CurrentBlock $currentBlock
                                if ($nextBlock -eq -1) {
                                    Write-Warning "Block chain ended early for $($entry.Name)"
                                    break
                                }
                                $currentBlock = $nextBlock
                            }
                        }
                    }
                } finally {
                    $outStream.Close()
                }
            }
        }
        Write-Host "`nExtraction complete to: $OutputDir"
    }
} finally {
    $reader.Close()
    $stfsFile.Close()
}
#endregion
