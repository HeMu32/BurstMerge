<#
.SYNOPSIS
  Inject Sony lens correction into DNG via OpcodeList3.
#>
param([string]$RefFile,[string]$DngFile,[string]$OutputFile='',[switch]$NoVig)
$ErrorActionPreference='Stop'
$exif="C:\MultiMediaTools\Bin\exiftool.exe"

function rx($a){
  $p=New-Object Diagnostics.ProcessStartInfo
  $p.FileName=$exif;$p.Arguments=$a;$p.UseShellExecute=$false
  $p.RedirectStandardOutput=$true;$p.RedirectStandardError=$true;$p.CreateNoWindow=$true
  $q=[Diagnostics.Process]::Start($p);$o=$q.StandardOutput.ReadToEnd();$q.WaitForExit();return $o
}

# Extract Sony params
$m=rx "-DistortionCorrParams -VignettingCorrParams `"$RefFile`""
$dp=@();$vp=@()
foreach($l in ($m -split "`n")){
  $l=$l.Trim()
  if($l -match '^Distortion Corr Params\s*:\s*(.+)'){$dp=@();foreach($x in ($matches[1]-split'[, ]')){$x=$x.Trim();if($x-ne''-and$x-match'^-?\d+$'){$dp+=[int]$x}}}
  if($l -match '^Vignetting Corr Params\s*:\s*(.+)'){$vp=@();foreach($x in ($matches[1]-split'[, ]')){$x=$x.Trim();if($x-ne''-and$x-match'^-?\d+$'){$vp+=[int]$x}}}
}
# Handle ARW (17 values: count+16 knots) and DNG (16 values)
if($dp.Length -ge 17 -and $dp[0] -le 32){$dp=$dp[1..16]}  # skip count prefix
if($vp.Length -ge 17 -and $vp[0] -le 32){$vp=$vp[1..16]}  # skip count prefix for vignette
if($dp.Length -ne 16){throw "Need 16 distortion params, got $($dp.Length)"}

Write-Host "Dist: $($dp.Length) Vign: $($vp.Length)"

# ---- Dist fit ----
# ALL LOWERCASE VARIABLES to avoid PS case-sensitivity bugs
$sd=1.0+$dp[15]/16384.0
$s0=0.0;$s1=0.0;$s2=0.0;$s3=0.0;$s4=0.0;$s5=0.0;$s6=0.0;$s7=0.0
$s8=0.0;$s9=0.0;$s10=0.0;$s11=0.0;$s12=0.0;$s13=0.0;$s14=0.0;$s15=0.0
$t0=0.0;$t1=0.0;$t2=0.0;$t3=0.0
for($i=0;$i-lt16;$i++){
  $rr=$i/15.0;$r2=$rr*$rr;$r4=$r2*$r2;$r6=$r4*$r2
  $yy=(1.0+$dp[$i]/16384.0)/$sd
  $s0+=1.0;$s1+=$r2;$s2+=$r4;$s3+=$r6
  $s4+=$r2;$s5+=$r2*$r2;$s6+=$r2*$r4;$s7+=$r2*$r6
  $s8+=$r4;$s9+=$r4*$r2;$s10+=$r4*$r4;$s11+=$r4*$r6
  $s12+=$r6;$s13+=$r6*$r2;$s14+=$r6*$r4;$s15+=$r6*$r6
  $t0+=$yy;$t1+=$r2*$yy;$t2+=$r4*$yy;$t3+=$r6*$yy
}
$mm=@(0.0)*20
$mm[0]=$s0;$mm[1]=$s1;$mm[2]=$s2;$mm[3]=$s3;$mm[4]=$t0
$mm[5]=$s4;$mm[6]=$s5;$mm[7]=$s6;$mm[8]=$s7;$mm[9]=$t1
$mm[10]=$s8;$mm[11]=$s9;$mm[12]=$s10;$mm[13]=$s11;$mm[14]=$t2
$mm[15]=$s12;$mm[16]=$s13;$mm[17]=$s14;$mm[18]=$s15;$mm[19]=$t3
for($c=0;$c-lt4;$c++){
  $mv=$mm[$c*5+$c];$mr=$c
  for($r=$c+1;$r-lt4;$r++){$av=$mm[$r*5+$c];if($av-lt0){$av=-$av};if($av-gt$mv){$mv=$av;$mr=$r}}
  if($mr-ne$c){for($j=$c;$j-le4;$j++){$t=$mm[$c*5+$j];$mm[$c*5+$j]=$mm[$mr*5+$j];$mm[$mr*5+$j]=$t}}
  $pv=$mm[$c*5+$c]
  for($r=$c+1;$r-lt4;$r++){$f=$mm[$r*5+$c]/$pv;for($j=$c;$j-le4;$j++){$mm[$r*5+$j]-=$f*$mm[$c*5+$j]}}
}
$kr=@(0.0)*4
for($i=3;$i-ge0;$i--){$ss=$mm[$i*5+4];for($j=$i+1;$j-lt4;$j++){$ss-=$mm[$i*5+$j]*$kr[$j]};$kr[$i]=$ss/$mm[$i*5+$i]}
Write-Host "WR: kr0=$($kr[0].ToString('F8')) kr1=$($kr[1].ToString('F8')) kr2=$($kr[2].ToString('F8')) kr3=$($kr[3].ToString('F8'))"

# ---- Vig fit (optional) ----
$hasv=0;$kv=@(0.0)*5
if(!$NoVig -and $vp.Length-ge15){
  $hasv=1;$n=15;$base=$vp[0]
  $aa=@(0.0)*25;$bb=@(0.0)*5
  for($i=0;$i-lt$n;$i++){
    $rr=$i/14.0;$gg=1.0+($vp[$i]-$base)/16384.0;$y1=$gg-1.0
    $r2=$rr*$rr;$r4=$r2*$r2;$r6=$r4*$r2;$r8=$r4*$r4;$r10=$r8*$r2
    $v=@($r2,$r4,$r6,$r8,$r10)
    for($j=0;$j-lt5;$j++){$bb[$j]+=$v[$j]*$y1;for($k=0;$k-lt5;$k++){$aa[$j*5+$k]+=$v[$j]*$v[$k]}}
  }
  $mat=@(0.0)*30
  for($i=0;$i-lt5;$i++){for($j=0;$j-lt5;$j++){$mat[$i*6+$j]=$aa[$i*5+$j]};$mat[$i*6+5]=$bb[$i]}
  for($c=0;$c-lt5;$c++){
    $mv=$mat[$c*6+$c];$mr=$c
    for($r=$c+1;$r-lt5;$r++){$av=$mat[$r*6+$c];if($av-lt0){$av=-$av};if($av-gt$mv){$mv=$av;$mr=$r}}
    if($mr-ne$c){for($j=$c;$j-le5;$j++){$t=$mat[$c*6+$j];$mat[$c*6+$j]=$mat[$mr*6+$j];$mat[$mr*6+$j]=$t}}
    $pv=$mat[$c*6+$c]
    for($r=$c+1;$r-lt5;$r++){$f=$mat[$r*6+$c]/$pv;for($j=$c;$j-le5;$j++){$mat[$r*6+$j]-=$f*$mat[$c*6+$j]}}
  }
  for($i=4;$i-ge0;$i--){$ss=$mat[$i*6+5];for($j=$i+1;$j-lt5;$j++){$ss-=$mat[$i*6+$j]*$kv[$j]};$kv[$i]=$ss/$mat[$i*6+$i]}
  Write-Host "FV: k0=$($kv[0].ToString('F6')) k1=$($kv[1].ToString('F6')) k2=$($kv[2].ToString('F6')) k3=$($kv[3].ToString('F6')) k4=$($kv[4].ToString('F6'))"
}

# ---- Build binary + inject ----
if(!$OutputFile){$d=Split-Path$DngFile-Parent;$b=[IO.Path]::GetFileNameWithoutExtension($DngFile);$OutputFile=Join-Path$d"${b}_corr.dng"}
$tmp=[IO.Path]::GetTempFileName()
$fs=[IO.File]::OpenWrite($tmp)
function w32($s,$v){$b=[BitConverter]::GetBytes($v);$s.WriteByte($b[3]);$s.WriteByte($b[2]);$s.WriteByte($b[1]);$s.WriteByte($b[0])}
function wd($s,$v){$b=[BitConverter]::GetBytes($v);$s.WriteByte($b[7]);$s.WriteByte($b[6]);$s.WriteByte($b[5]);$s.WriteByte($b[4]);$s.WriteByte($b[3]);$s.WriteByte($b[2]);$s.WriteByte($b[1]);$s.WriteByte($b[0])}
$n2=1;if($hasv){$n2=2};w32 $fs $n2
w32 $fs 1;w32 $fs 0x01030000;w32 $fs 1;w32 $fs 68;w32 $fs 1
wd $fs $kr[0];wd $fs $kr[1];wd $fs $kr[2];wd $fs $kr[3]
wd $fs 0.0;wd $fs 0.0;wd $fs 0.5;wd $fs 0.5
if($hasv){w32 $fs 3;w32 $fs 0x01030000;w32 $fs 1;w32 $fs 56
  wd $fs $kv[0];wd $fs $kv[1];wd $fs $kv[2];wd $fs $kv[3];wd $fs $kv[4];wd $fs 0.5;wd $fs 0.5}
$fs.Close()

$doc=[IO.File]::ReadAllBytes($DngFile);$opc=[IO.File]::ReadAllBytes($tmp)
Remove-Item $tmp -Force

$found=$null;$io=[BitConverter]::ToUInt32($doc,4)
while($io-gt0-and$io-lt$doc.Length){
  $nu=[BitConverter]::ToUInt16($doc,$io)
  for($i=0;$i-lt$nu;$i++){$e=$io+2+$i*12;$tt=[BitConverter]::ToUInt16($doc,$e)
    if($tt-eq0x014A){$ct=[BitConverter]::ToUInt32($doc,($e+4));$vl=[BitConverter]::ToUInt32($doc,($e+8));$so=$vl;if($ct-ne1){$so=[BitConverter]::ToUInt32($doc,$vl)}
      $sn=[BitConverter]::ToUInt16($doc,$so)
      for($k=0;$k-lt$sn;$k++){$se=$so+2+$k*12;$st=[BitConverter]::ToUInt16($doc,$se)
        if($st-eq0xC74E){$do=[BitConverter]::ToUInt32($doc,($se+8));$co=$se+4;$os=[BitConverter]::ToUInt32($doc,($se+4));$found=@($do,$co,$os)}}}}
  $io=[BitConverter]::ToUInt32($doc,($io+2+$nu*12))}
if(!$found){throw "OpcodeList3 not found"}
if($opc.Length-gt$found[2]){throw "Opcode $($opc.Length)B exceeds $($found[2])B"}
for($j=0;$j-lt$opc.Length;$j++){$doc[$found[0]+$j]=$opc[$j]}
for($j=$opc.Length;$j-lt$found[2];$j++){$doc[$found[0]+$j]=0}
$nc=$opc.Length;$doc[$found[1]]=$nc-band0xFF;$doc[$found[1]+1]=($nc-shr8)-band0xFF
$doc[$found[1]+2]=($nc-shr16)-band0xFF;$doc[$found[1]+3]=($nc-shr24)-band0xFF
[IO.File]::WriteAllBytes($OutputFile,$doc)

$vv=rx "-OpcodeList3 -n `"$OutputFile`""
if($vv-match'Binary data (\d+) bytes'){Write-Host "OpcodeList3 = $($matches[1]) bytes"}
Write-Host "Done: $OutputFile"
