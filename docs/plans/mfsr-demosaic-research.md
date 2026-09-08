# MFSR + Demosaic (Bayer) 研究笔记与开发指导

> 用途:记录 2x 超分辨率 + 去马赛克 (Bayer, 非 DNN) 的经典理论与本项目的落地路线。
> 关联:当前 SR 实现的审计结论见会话记录 —— 现有实现是"整数基对齐 + 64px 瓦片 ±0.5/0.25 量化残差 + 1.0/0.05 两档权重",结构上偏向软输出。

## 一、经典观测模型 (MFSR 基础理论)

```
y_k = D_k B_k W_k z + n_k
```

- `y_k`:第 k 帧低分辨率图;`z`:待求高分辨率图;`W_k`:几何变形(运动);`B_k`:模糊(光学+传感器);`D_k`:下采样;`n_k`:噪声。
- 本质是病态逆问题,可恢复的前提是各帧之间存在**亚像素位移**。
- Bayer 额外有一层 CFA 马赛克采样:每颜色通道只在 1/4 位置有值("规则欠采样")。理论上 2x 放大即可补齐各通道全密度采样。

## 二、关键参考

### 经典 MFSR
- Tsai & Huang (1984) — 频率域 SR 开山之作。
- Irani & Peleg (1991, 1993) — 迭代反投影法 (IBP)。
- Elad & Feuer (1997, IEEE TIP) — MAP 框架。
- Hardie, Barnard, Armstrong (1997, IEEE TIP) — 联合运动估计 + SR。
- **Farsiu et al. (2004, IEEE TIP)** — L1 数据项 + 双边全变分 (BTV) 正则,对噪声/运动误差鲁棒。最著名的经典实现。
- Protter et al. (2009, IEEE TIP) — NLM-SR。
- **Takeda, Farsiu, Milanfar (2007, IEEE TIP)** — steering kernel regression,是 Wronski 算法融合步骤的理论根基。
- Zibetti & Mayer (2007) — 鲁棒联合 SR。
- 综述:Borman & Stevenson (1998);Park, Park, Kang (2003, IEEE Signal Proc. Mag.);Yue et al. (2016, Signal Processing)。
- 经典方法类别:频域法、IBP、POCS、MAP+正则 (Tikhonov/TV/BTV)、稀疏编码、核回归 (KSR)、非局部均值。工程上易落地的是 **IBP、MAP+BTV、KSR** 三系。

### 计算摄影 / 手持连拍 (最贴合本项目)
- **Google HDR+** — Hasinoff et al., SIGGRAPH Asia 2016 (arXiv/PDF: hdrplusdata.org)。恒定曝光 Bayer RAW 连拍;FFT 块匹配对齐 + 混合 2D/3D Wiener 滤波合并。IPOL 复现:hdrplus-python (amonod)。
- ⭐ **Handheld Multi-Frame Super-Resolution (Wronski 2019, arXiv:1905.03277, SIGGRAPH 2019)** — 直接从 CFA RAW 连拍帧生成完整 RGB,**不含显式 demosaic**;利用手持自然抖动提供亚像素偏移;对齐(全局单应 + 分块平移细化)后,用自适应核回归 (steering kernel,依局部梯度张量导向)+ 迭代加权去离群融合,再做去卷积锐化。**本项目 hdr-plus-swift 即此算法移植,是主锚点。**
- Lafenetre et al. (arXiv:2303.05879) — 同款 KSR 思路扩展到多曝光卫星图。
- Burst 去噪 (同管线):Mildenhall 2018 (KPN, arXiv:1712.02327);Godard et al. (Deep Burst Denoising, arXiv:1712.05790);Xia 2020 (Basis Prediction, arXiv:1912.04421);Li 2022 (arXiv:2205.04721)。

### 深度学习 Burst SR (参考,本路线不用 DNN)
- Bhat et al., Deep Burst Super-Resolution (CVPR 2021, arXiv:2101.10997) — 光流对齐 + attention 融合,BurstSR 数据集。
- Bhat et al., Deep Reparametrization of MFSR and Denoising (ICCV 2021, arXiv:2108.08286)。
- NTIRE 2021 Burst SR Challenge (arXiv:2106.03839)。

### Bayer / RAW 专题
- **Farsiu et al. 2006 — Multiframe Demosaicing + SR**:正是"Bayer 多帧去马赛克 + SR"的经典实现。
- RAW 域注意点:每颜色通道是周期性亚栅格采样;帧间亚像素位移可实现每通道全密度重建,**2x 即可"去马赛克+SR"二合一**;对齐必须在 RAW/float 域、按通道理解 CFA 相位,对 mosaic 数据直接插值会破坏相位;噪声模型 (光子散粒 + 读出) 是 RAW 域处理核心。

### 多曝光 / 数据集与评测
- NEBI "Burst Image Super-Resolution with Base Frame Selection" (CVPRW 2024, arXiv:2406.17869) — 非均匀曝光 burst,基帧选择网络。
- 数据集:BurstSR (Bhat 2021)、RealBSR (ICCV 2023)、Real-RawVSR (ECCV 2022)、hdrplusdata.org、IR275K、NTIRE 2025 Efficient Burst HDR (arXiv:2505.12089)。
- 评测:PSNR/SSIM + LPIPS;分辨率卡 ISO 12233;合成评估需校准噪声模型。

### 核心必读 4 篇
Wronski 2019 (1905.03277)、Hasinoff 2016 (HDR+)、Farsiu 2004 (鲁棒 MFSR)、Bhat 2021 (2101.10997)。Bayer 专题加 Farsiu 2006 和 Takeda 2007。

## 三、关键洞察:2x SR 天然兼容 Bayer

Bayer 每个颜色通道在 LR 图上只采样 1/4 位置。放大 2x 后(HR 网格 = 2×2 块对应一个 LR 像素),4 个 Bayer 相位正好展开成 HR 网格的 4 个亚像素位置:

```
LR 像素 → HR 2×2 块:
  R  → (0,0)
  Gb → (0,1)
  Gr → (1,0)
  B  → (1,1)
```

只要帧间有亚像素位移,把各帧按对齐结果叠加到 HR 网格上,每个 HR 像素位点就能被某一帧填上恰好一个颜色值 → 得到"HR 分辨率下的完整 Bayer mosaic" → 最后做一次常规 demosaic。SR 与 demosaic 是**接力而非叠加**,不引入二重插值伪影。2x 是 Bayer 最自然的放大倍数。

## 四、两条低复杂度路线 (非 DNN)

### 路线 A(推荐,模块化):逐通道 MFSR → 组装 HR mosaic → 一次 demosaic
1. 把 Bayer 拆成 4 个相位子图(各相位是规则亚采样格点)。
2. 对每个相位做 MFSR(只补该相位密度):先 shift-and-add,再跑 1~2 轮 IBP 校正。
3. 按相位位置贴回 HR 网格,得到 HR Bayer mosaic。
4. 用现有 demosaic 代码一次性解出 HR RGB。

好处:复用现有 demosaic;每步都是几十行的经典算法;相位间互不干扰。

### 路线 B(已有,微调即可):Wronski 式直接重建 RGB
hdr-plus-swift 现在做法:每个 HR 像素从各帧收集支持域内样本,用梯度导向的自适应核回归 (steering kernel) 加权融合,自带去离群。无显式 demosaic,把 SR+demosaic 合二为一(即 Takeda 2007)。保持方向,重点是把"等分辨率合并"改成"2x HR 重建"。

## 五、非 DNN 算法选型(按复杂度递增)

| 算法 | 复杂度 | 特点 |
|---|---|---|
| Shift-and-add (drizzle) | ★ | 按亚像素位移投影到 HR 网格加权平均;最简基线;噪声随帧数 √ 下降 |
| IBP (Irani & Peleg 1991) | ★★ | shift-and-add 基础上迭代:模拟 LR→算残差→反投影修正;1~2 轮即可,锐度明显提升 |
| Steering kernel regression (Takeda 2007 / Wronski) | ★★ | 梯度导向核,边缘不糊;本项目已有 |
| MAP + BTV (Farsiu 2004, 2006) | ★★★ | L1 数据项 + 双边 TV 正则,梯度下降迭代;Farsiu 2006 正是"多帧 Bayer demosaic+SR",可直接照论文实现 |

## 六、对本项目 (BurstMerge) 的开发指导

- 核心对标已是 Wronski 2019 + HDR+,参考系正确。经典补充:BTV 正则 / L1 鲁棒项提升抗局部运动;steering kernel 的梯度导向核与迭代重加权去离群 (Takeda 2007、Farsiu 2004、IPOL HDR+)。
- **对齐精度决定增益**:子像素位移是信息源;建议验证相位相关/FFT 块匹配 (HDR+) 与当前金字塔 SAD 的效果差距;**对齐误差 > 0.5px 时 SR 增益趋零**。
- **Bayer 相位意识**:对齐/插值不跨 CFA 相位;若在 float mosaic 域工作按通道分组;参考 Farsiu 2006 跨帧彩色融合。
- **噪声/曝光归一化**:合并前按 EV 归一(已有);RAW 域物理噪声模型 (Poisson-Gaussian) 用于权重/门控会更稳。
- **多曝光输入**:参考 NEBI 基帧选择思路 —— 低 EV 帧不一定是最佳基帧,可按每帧质量选择/加权。
- 盲退化:KBNet 的核估计 + 对齐联合可借鉴(未来)。

## 七、与现有实现 (审计结论) 的对照

现有 2x SR 实现:"整数基对齐 + 64px 瓦片 ±0.5/0.25 量化残差搜索 + 1.0/0.05 两档权重",在**全分辨率插值后的 RGB** 上做。审计结论:结构上偏向软输出。

对照本笔记,现有实现偏离了"在 RAW/float mosaic 域按 CFA 相位工作"的原则:
- 对齐在 demosaic 之后(已破坏相位信息),且基对齐是整数,亚像素靠量化残差旁路;
- 重建在插值后的 RGB 上双三次/双线性采样,等于叠加二重插值伪影,而非"接力式"一次 demosaic。

若按路线 A 改造:逐相位 shift-and-add/IBP 在 RAW float 域完成密度补齐 → 组装 HR mosaic → 一次 demosaic,可同时满足"2x SR 有效"与"不引入二重插值伪影"。