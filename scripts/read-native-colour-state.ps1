param([Parameter(Mandatory=$true)][int]$GameProcessId,[Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
if(-not [IO.Path]::IsPathFullyQualified($OutputPath)){throw 'Output path must be absolute'}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NativeColourRead {
 [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access,bool inherit,int id);
 [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr process,IntPtr address,byte[] data,UIntPtr size,out UIntPtr read);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
 public static byte[] Bytes(IntPtr process,long address,int count) { byte[] b=new byte[count];UIntPtr n;if(address<=0||count<0||!ReadProcessMemory(process,new IntPtr(address),b,new UIntPtr((uint)count),out n)||n.ToUInt64()!=(ulong)count) throw new Exception("read failed");return b; }
}
'@
$expected='F:\Steam\steamapps\common\Space Marine 2\client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe'
$game=Get-CimInstance Win32_Process | Where-Object {$_.ProcessId -eq $GameProcessId -and $_.ExecutablePath -eq $expected}
if(@($game).Count -ne 1){throw 'Expected exact owned Retail process'}
$gameProcess=Get-Process -Id $GameProcessId
$imageBase=$gameProcess.MainModule.BaseAddress.ToInt64()
$readHandle=[NativeColourRead]::OpenProcess(0x1010,$false,$GameProcessId)
function Bytes([long]$address,[int]$length){,[NativeColourRead]::Bytes($readHandle,$address,$length)}
function Pointer([long]$address){[BitConverter]::ToInt64((Bytes $address 8),0)}
try {
 $peOffset=[BitConverter]::ToInt32((Bytes ($imageBase+0x3c) 4),0)
 if($peOffset -lt 0 -or $peOffset -gt 0x100000 ){throw 'Unknown native build'}
 # Globals per verified build (reference 2026-10-05, port 2026-10-21; mod/src/GameBuildTable.inc).
 $builds=@{0x6ac3e716=@{list=0x4f7c6c8;hdr=0x4f9d010;pipeline=0x4faf840;scale=0x4faff88};0x6ad10fcb=@{list=0x4f7c688;hdr=0x4f9cee0;pipeline=0x4faf800;scale=0x4faff48}}
 $stamp=[int64][BitConverter]::ToUInt32((Bytes ($imageBase+$peOffset+8) 4),0)
 if(-not $builds.ContainsKey($stamp)){throw 'Unknown native build'}
 $g=$builds[$stamp]
 $viewList=Pointer ($imageBase+$g.list)
 $count=[BitConverter]::ToInt32((Bytes ($viewList+0x29968) 4),0)
 if($count -lt 2 -or $count -gt 64){throw 'Expected bounded main/PIP list'}
 $views=@(for($slot=0;$slot -lt $count;$slot++) {
  $entry=$viewList+0x168+$slot*0xa60
  $header=Bytes $entry 0x18c
  $flags=[BitConverter]::ToUInt64($header,0)
  if(($flags -band 0x40) -eq 0 -and (($flags -band 0x50000044) -ne 0 -or ($flags -band 0x20000) -eq 0)){continue}
  $index=[BitConverter]::ToInt32($header,0x188)
  if($index -lt 0 -or $index -gt 1){throw 'Unexpected HDR index'}
  $hdr=Bytes ($imageBase+$g.hdr+$index*0x198) 0x198
  $curve=@(for($i=0;$i -lt 7;$i++){[BitConverter]::ToSingle($hdr,0x84+$i*4)})
  $exposure=@(foreach($offset in @(0x174,0x178,0x180,0x184,0x188,0x18c)){[pscustomobject]@{offset=('0x{0:x}' -f $offset);value=[BitConverter]::ToSingle($hdr,$offset)}})
  $components=@(foreach($offset in @(0x5d8,0x5e0,0x5e8,0x5f0,0x5f8,0x600,0x608,0x610,0x618,0x620,0x628,0x630,0x638,0x640)) {
   $component=Pointer ($imageBase+$g.pipeline+$offset)
   $mask=[BitConverter]::ToUInt32((Bytes ($component+8+$index*4) 4),0)
   [pscustomobject]@{offset=('0x{0:x}' -f $offset);object=$component;mask=$mask;enabled=[bool]($mask -band 2)}
  })
  $gradingObject=Pointer ($imageBase+$g.pipeline+0x628)
  $grading=@(foreach($offset in @(0x58,0x5c,0x60,0x64,0x68)) { [pscustomobject]@{offset=('0x{0:x}' -f $offset);value=[BitConverter]::ToSingle((Bytes ($gradingObject+$index*0x5c+$offset) 4),0)} })
  $gradingMode=[BitConverter]::ToUInt32((Bytes ($gradingObject+$index*0x5c+0x68) 4),0)
  $gradingScaleEnabled=(Bytes ($imageBase+$g.scale+$index*0x210) 1)[0]
  $gradingScale=[BitConverter]::ToSingle((Bytes ($imageBase+$g.scale+4+$index*0x210) 4),0)
  $again=Bytes $entry 0x18c
  if([BitConverter]::ToUInt64($again,0) -ne $flags -or [BitConverter]::ToInt32($again,0x188) -ne $index){throw 'View identity changed during diagnostic read'}
  [pscustomobject]@{slot=$slot;entry=$entry;flags=$flags;pip=[bool]($flags -band 0x40);hdr_index=$index;curve=$curve;exposure=$exposure;components=$components;outer_grading=$grading;outer_mode=$gradingMode;outer_scale_enabled=$gradingScaleEnabled;outer_scale=$gradingScale}
 })
 if((Pointer ($imageBase+$g.list)) -ne $viewList -or [BitConverter]::ToInt32((Bytes ($viewList+0x29968) 4),0) -ne $count){throw 'View list changed during diagnostic read'}
 if(@($views | Where-Object pip).Count -ne 1 -or @($views | Where-Object {-not $_.pip}).Count -ne 1){throw 'Expected unique main/PIP diagnostic views'}
 [ordered]@{process=$GameProcessId;module_base=$imageBase;view_list=$viewList;diagnostic_only=$true;atomic_frame_identity=$false;views=$views} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath
 "Saved read-only native colour state: $OutputPath"
} finally {[void][NativeColourRead]::CloseHandle($readHandle)}
