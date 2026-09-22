# =============================================================================
# provision-device.ps1 —— 产线：把**这一台**设备的凭证写进它的 flash，并回读校验
#
# ⚠️ 本文件必须存为 **UTF-8 with BOM**。原因（踩过）：Windows PowerShell 5.1 对**没有 BOM** 的
#    .ps1 按系统本地代码页（中文 Windows 上是 GBK）解码，中文注释与中文字符串会变成乱码，
#    字符串里的引号被吃掉后报出一串 "Unexpected token '}'" —— 报错位置指向的是**无辜的行**，
#    看起来像语法写错了。改动本文件后请确认首三字节仍是 EF BB BF。
#
# 对应设计 §8.5「产线写入流程」的第 2、3 步（烧固件是可选的第 1 步）。
#
# 为什么要有这个脚本：
#   量产形态是「**一份通用固件 + 每台一份凭证**」。固件对每台机器完全一样，**每台机器唯一的
#   差别就是 creds 分区里那份镜像**。所以产线上最容易出的事故不是"烧不进去"，而是：
#     · 把 A 台的镜像写进了 B 台（两台机器抢同一个身份，上线后互相踢线）；
#     · 分区写空了 / 写坏了（设备要么静默回退、要么拒绝上网，现场很难判断原因）；
#     · 全片擦除与写凭证的顺序搞反（擦除会把凭证一起抹掉，成品却是"烧过了"的样子）。
#   这个脚本把这三件事变成**做不成**：镜像必须能在清单里对上这台 SN、必须自身完整（magic/
#   版本/长度/CRC 逐项校验）、写完必须**回读**并与本地逐字节一致。
#
# 用法（一台一次）：
#   powershell -ExecutionPolicy Bypass -File provision-device.ps1 `
#       -Port COM12 -Sn KORVO2-9001 -CredsDir E:\prod\creds
#
#   # 连固件一起烧（首次 / 换分区表：`-Erase` 会先全片擦除，顺序由脚本保证）
#   powershell -ExecutionPolicy Bypass -File provision-device.ps1 `
#       -Port COM12 -Sn KORVO2-9001 -CredsDir E:\prod\creds `
#       -BuildDir E:\workspace\rainmaker-oneye\embedded\esp-adf\output\.build\korvo2_oneye-devcreds -Erase
#
#   # 产线留档：每次调用追加一行 PASS/FAIL
#   ... -LogFile E:\prod\provision-log.csv
#
# `-CredsDir` 是 `mkcreds -zip <批量签发 ZIP> -out <目录>` 的输出目录，里面应有：
#   creds-manifest.csv        （sn,node_id,offset,size,cert_fingerprint_sha256,image_crc32,image_sha256,file）
#   <SN>\creds.bin            （已补满 16 KB 的分区镜像）
#
# 退出码：0 = PASS；1 = 没有写成功或回读不一致（**不要**把非 0 当"差不多好了"）；
#         2 = 参数/环境问题（没有真正动过 flash）。
# =============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [Parameter(Mandatory = $true)][string]$Sn,
    [Parameter(Mandatory = $true)][string]$CredsDir,
    [string]$Offset = '0x510000',
    [int]$Size = 16384,
    [string]$BuildDir = '',
    [switch]$Erase,
    [string]$Idf = 'E:\esp\esp-idf-5.5.5',
    [int]$Baud = 460800,
    [string]$LogFile = ''
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- 输出与留档
function Write-Log([string]$result, [string]$note, [string]$node = '', [string]$crc = '', [string]$sum = '') {
    if (-not $LogFile) { return }
    $row = [pscustomobject]@{
        time    = (Get-Date).ToString('s')
        port    = $Port
        sn      = $Sn
        node_id = $node
        crc32   = $crc
        sha256  = $sum
        result  = $result
        note    = $note
    }
    if (-not (Test-Path $LogFile)) {
        ($row | ConvertTo-Csv -NoTypeInformation)[0] | Set-Content -Path $LogFile -Encoding UTF8
    }
    ($row | ConvertTo-Csv -NoTypeInformation)[1] | Add-Content -Path $LogFile -Encoding UTF8
}
function Fail([string]$msg, [int]$code = 1, [string]$note = '') {
    Write-Host "[provision] FAIL: $msg" -ForegroundColor Red
    Write-Log 'FAIL' ($(if ($note) { $note } else { $msg }))
    exit $code
}
function FailArgs([string]$msg) { Write-Host "[provision] 参数/环境问题: $msg" -ForegroundColor Yellow; exit 2 }

# ---------------------------------------------------------------- 镜像自身的校验
# CRC32/IEEE（与镜像头里的取值口径一致：只覆盖 payload，不含分区末尾的 0xFF 填充）。
#
# ⚠️ 两个常量**故意写成十进制**（4294967295 / 3988292384，即 0xFFFFFFFF / 0xEDB88320）。
#    坑在哪：**Windows PowerShell 5.1 把 ≥ 0x80000000 的十六进制字面量当成有符号 Int32**
#    （0xFFFFFFFF → -1、0xEDB88320 → -306674912），于是"掩码"变成 -1 而失效、末步
#    `[uint32](...)` 抛 "Cannot convert value "-1" to type "System.UInt32""；
#    而 PowerShell 7 把它当无符号值 —— **同一段代码在 7 上完全正常**，所以这类问题只有真在
#    5.1（产线机器就是 5.1）上跑才会暴露。写十进制则两个版本一致（实测向量 123456789 ⇒ cbf43926）。
function Get-Crc32([byte[]]$data, [int]$start, [int]$count) {
    [int64]$mask = 4294967295
    [int64]$poly = 3988292384
    [int64]$crc = $mask
    for ($i = $start; $i -lt ($start + $count); $i++) {
        $crc = ($crc -bxor [int64]$data[$i]) -band $mask
        for ($b = 0; $b -lt 8; $b++) {
            if ($crc -band 1) { $crc = (($crc -shr 1) -bxor $poly) -band $mask }
            else { $crc = ($crc -shr 1) -band $mask }
        }
    }
    return [uint32](($crc -bxor $mask) -band $mask)
}

# 独立解析镜像（不看清单也会做）：magic / 版本 / payload 长度 / CRC / JSON 身份。
# 返回 @{ NodeID=..; SN=..; MAC=..; CRC=..; Payload=.. } 或抛错。
function Read-Image([byte[]]$img, [string]$what) {
    if ($img.Length -lt 20) { throw "$what 只有 $($img.Length) B，连 20 B 的头都不够" }
    $blank = $true
    for ($i = 0; $i -lt 8; $i++) { if ($img[$i] -ne 0xFF) { $blank = $false; break } }
    if ($blank) { throw "$what 是空的（全 0xFF）—— 这个分区还没写过" }
    $magic = [System.Text.Encoding]::ASCII.GetString($img, 0, 8)
    if ($magic -ne 'ONEYECR1') {
        throw "$what 的前 8 字节不是 ONEYECR1 —— 这不是一份凭证镜像（实际 '$magic'）"
    }
    $ver = [System.BitConverter]::ToUInt16($img, 8)
    if ($ver -ne 1) { throw "$what 的镜像版本是 $ver，本脚本只认 1（固件/工具版本不匹配）" }
    $len = [System.BitConverter]::ToUInt32($img, 12)
    if ($len -eq 0 -or ($len + 20) -gt $img.Length) { throw "$what 的 payload 长度 $len 不合理（文件 $($img.Length) B）" }
    $crcWant = [System.BitConverter]::ToUInt32($img, 16)
    $crcGot = Get-Crc32 $img 20 ([int]$len)
    if ($crcGot -ne $crcWant) {
        throw ("{0} 的 CRC 不符（头里写 {1:x8}，实算 {2:x8}）—— 文件坏了" -f $what, $crcWant, $crcGot)
    }
    $json = [System.Text.Encoding]::UTF8.GetString($img, 20, [int]$len)
    $o = $json | ConvertFrom-Json
    if (-not $o.node_id) { throw "$what 的 payload 里没有 node_id" }
    return @{ NodeID = [string]$o.node_id; SN = [string]$o.sn; MAC = [string]$o.mac; CRC = $crcGot; Payload = [int]$len }
}

# ---------------------------------------------------------------- 1. 对清单
if (-not (Test-Path $CredsDir)) { FailArgs "凭证目录不存在：$CredsDir" }
$CredsDir = (Resolve-Path $CredsDir).Path
$mfPath = Join-Path $CredsDir 'creds-manifest.csv'
if (-not (Test-Path $mfPath)) {
    FailArgs "找不到 $mfPath —— 没有清单就无法证明这份镜像是这台机器的（别绕过这一步）"
}
$rows = @(Import-Csv -Path $mfPath)
if ($rows.Count -eq 0) { FailArgs "$mfPath 里没有数据行" }

# 清单格式体检：按**列名**取值，但列名不存在时要立刻说清"是清单格式不对"，而不是让后面报出
# "这行没有 sha256" 这种把人引偏的话（旧版清单把证书指纹写在 `crc32` 列下，正是这种情况）。
$cols = @($rows[0].PSObject.Properties.Name)
foreach ($need in @('sn', 'node_id', 'status', 'offset', 'size', 'cert_fingerprint_sha256', 'image_crc32', 'image_sha256')) {
    if ($cols -notcontains $need) {
        FailArgs "$mfPath 缺少列 '$need'（实际列：$($cols -join ', ')）—— 这是旧格式或别的工具产出的清单，请用当前版本的 mkcreds 重新生成"
    }
}

$row = $rows | Where-Object { $_.sn -ceq $Sn } | Select-Object -First 1
if (-not $row) {
    $row = $rows | Where-Object { $_.sn -ieq $Sn } | Select-Object -First 1
    if ($row) { Write-Host "[provision] 注意：清单里的 SN 是 '$($row.sn)'，与 -Sn '$Sn' 仅大小写不同（按它继续）" -ForegroundColor Yellow }
}
if (-not $row) {
    $have = ($rows | ForEach-Object { "$($_.sn)($($_.status))" }) -join ', '
    FailArgs "清单里没有 SN=$Sn 这一台。清单里有：$have"
}
if ($row.status -ne 'issued') {
    $why = if ($row.detail) { "：$($row.detail)" } else { '（清单没写原因）' }
    FailArgs "SN=$Sn 在清单里的状态是 '$($row.status)'$why —— 这台没有可烧的镜像，别拿别的台的文件顶替"
}
if ($row.offset -ne $Offset) {
    FailArgs "清单里的偏移是 $($row.offset)，脚本用的是 $Offset —— 分区布局不一致，先对齐 partitions.csv"
}
if ([int]$row.size -ne $Size) {
    FailArgs "清单里的分区大小是 $($row.size)，脚本用的是 $Size —— 同上"
}

$imagePath = Join-Path $CredsDir (Join-Path $row.sn 'creds.bin')
if (-not (Test-Path $imagePath)) { FailArgs "找不到镜像文件：$imagePath" }
$img = [System.IO.File]::ReadAllBytes($imagePath)
if ($img.Length -ne $Size) { FailArgs "镜像大小是 $($img.Length) B，应为 $Size B（未补满分区的镜像不要拿来烧）" }
$sumLocal = (Get-FileHash -Path $imagePath -Algorithm SHA256).Hash.ToLower()
if ($sumLocal -ne $row.image_sha256.ToLower()) {
    FailArgs "镜像文件与清单对不上（清单 $($row.image_sha256)，实际 $sumLocal）—— 目录被换过/改过，停下"
}
$info = Read-Image $img '镜像文件'
if ($info.NodeID -ne $row.node_id) {
    FailArgs "镜像里的 node_id='$($info.NodeID)' 与清单的 '$($row.node_id)' 不一致 —— 这份文件不是这一台的"
}
if ($info.SN -and $info.SN -ne $row.sn) {
    FailArgs "镜像里的 sn='$($info.SN)' 与清单的 '$($row.sn)' 不一致"
}

$crcHex = ('{0:x8}' -f $info.CRC)
if ($row.image_crc32 -and $crcHex -ne $row.image_crc32.ToLower()) {
    FailArgs "镜像 CRC 与清单不符（清单 $($row.image_crc32)，实算 $crcHex）"
}

Write-Host "[provision] 本台身份（来自镜像内部，不是来自文件名）:" -ForegroundColor Cyan
Write-Host "            SN      = $($row.sn)"
Write-Host "            node_id = $($info.NodeID)      <- 设备上线后门卫侧应看到这个名字"
Write-Host "            MAC     = $(if ($info.MAC) { $info.MAC } else { '（未采集，按 SN 管理）' })"
Write-Host "            镜像    = $Size B（payload $($info.Payload) B，CRC32 $crcHex）"
Write-Host "            sha256  = $sumLocal"
Write-Host "            端口    = $Port"

# ---------------------------------------------------------------- 2. esptool 环境
$exportPs1 = Join-Path $Idf 'export.ps1'
if (-not (Test-Path $exportPs1)) { FailArgs "找不到 IDF 的 export.ps1：$exportPs1（用 -Idf 指定）" }
& $exportPs1 | Out-Null
$py = if ($env:IDF_PYTHON_ENV_PATH) { Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe' } else { 'python' }
if (-not (Get-Command $py -ErrorAction SilentlyContinue)) { FailArgs "找不到 python：$py" }

function Invoke-Esptool([string[]]$esptoolArgs, [string]$what) {
    Write-Host "[provision] $what" -ForegroundColor Cyan
    & $py -m esptool --chip esp32s3 -p $Port @esptoolArgs
    if ($LASTEXITCODE -ne 0) {
        Fail "$what 失败（esptool 退出码 $LASTEXITCODE）。常见原因：串口被占用（先关掉串口监视/串口助手）、没进下载模式、线材/驱动问题" 1 $what
    }
}

# ---------------------------------------------------------------- 3. 可选的固件烧写（顺序在这里保证）
if ($Erase) {
    if (-not $BuildDir) {
        Write-Host '[provision] 注意：-Erase 会把凭证分区一起擦掉；本脚本在擦除后会重新写入凭证，顺序是安全的，但中途失败时这台机器会停在"擦干净"的状态' -ForegroundColor Yellow
    }
    Invoke-Esptool @('-b', "$Baud", 'erase_flash') '全片擦除'
}
if ($BuildDir) {
    if (-not (Test-Path $BuildDir)) { FailArgs "构建目录不存在：$BuildDir" }
    $boot = Join-Path $BuildDir 'bootloader\bootloader.bin'
    $part = Join-Path $BuildDir 'partition_table\partition-table.bin'
    $app = Join-Path $BuildDir 'korvo2_oneye.bin'
    $sr = Join-Path $BuildDir 'srmodels\srmodels.bin'
    foreach ($f in @($boot, $part, $app, $sr)) {
        if (-not (Test-Path $f)) { FailArgs "缺少构建产物：$f" }
    }
    Invoke-Esptool @(
        '-b', "$Baud", '--before', 'default_reset', '--after', 'hard_reset',
        'write_flash', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', 'detect',
        '0x0', $boot, '0x8000', $part, '0x10000', $app, '0x210000', $sr
    ) '写入通用固件（4 段，所有机器相同）'
}

# ---------------------------------------------------------------- 4. 写这一台的凭证
Invoke-Esptool @(
    '-b', "$Baud", '--before', 'default_reset', '--after', 'hard_reset',
    'write_flash', $Offset, $imagePath
) "写入凭证分区 $Offset <- $imagePath"

# ---------------------------------------------------------------- 5. 回读校验（不可省）
$tmp = [System.IO.Path]::GetTempFileName()
try {
    Invoke-Esptool @('-b', "$Baud", 'read_flash', $Offset, "$Size", $tmp) "回读 $Offset（$Size B）"
    $back = [System.IO.File]::ReadAllBytes($tmp)
    if ($back.Length -ne $Size) { Fail "回读长度 $($back.Length) B ≠ $Size B" 1 '回读长度不符' }

    $sumBack = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash.ToLower()
    if ($sumBack -ne $sumLocal) {
        Fail "回读内容与写入的镜像不一致（写 $sumLocal，读回 $sumBack）—— 这台机器上的凭证不可信，重写；反复不一致要查 flash" 1 '回读 sha256 不符'
    }
    # 再用"镜像自己"的话语复核一遍：能解析 = 设备启动时也能解析。
    $binfo = Read-Image $back '回读内容'
    if ($binfo.NodeID -ne $info.NodeID) { Fail "回读解析出的 node_id='$($binfo.NodeID)' ≠ '$($info.NodeID)'" 1 '回读身份不符' }
} finally {
    Remove-Item -Path $tmp -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "[provision] PASS — SN=$($row.sn) node_id=$($info.NodeID) 已写入 $Offset，回读逐字节一致" -ForegroundColor Green
Write-Host "[provision] 线上还差最后一步（人工/上位机）：让这台设备联网，然后核对" -ForegroundColor Green
Write-Host "            设备状态面 cloud.link_up=true transport=mqtt-tls" -ForegroundColor Green
Write-Host "            门卫侧 Client($($info.NodeID), username=$($info.NodeID), connected=true)" -ForegroundColor Green
Write-Log 'PASS' "offset=$Offset size=$Size" $info.NodeID $crcHex $sumLocal
exit 0
