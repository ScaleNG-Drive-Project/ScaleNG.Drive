# NGX init-failure elimination ledger (HISTORICAL, SUPERSEDED)

> **HISTORICAL / SUPERSEDED — condensed record of the pre-success era.**
> Phase label (exact): **Pre-DLSS initialization**.
> Boundary run: **`20261007T165257Z`** (2026-10-07) — first verified
> `Init_Ext SUCCEEDED` + feature created + evals #1..1800+
> (STATUS.md:1962-1968). Live sections stay in `docs/STATUS.md`; this file
> condenses them with pointers.

Baseline codes (STATUS.md:1745-1752, 1780-1787): first attempt 182725Z;
classic `NVSDK_NGX_D3D12_Init` → `0xBAD00001` FeatureNotSupported;
`Init_Ext` → `0xBAD00002` PlatformError (vendored 1.5.0 AND current headers
agree); `GetFeatureRequirements` rr=1 SUCCESS yet supported=0 on the RTX 3050;
snippet self-reports API `0x13`. Discovery-supported yet bring-up-refused is
the fact the era had to explain.

| # | Hypothesis / variable | Exact matrix | Verdict | STATUS pointer |
|---|---|---|---|---|
| 1 | AppId | 241534720 / 0 / 608174073 (OptiScaler-generic) — identical codes | ELIMINATED (later noted vacuous: docs say use 0; `Denied` never observed) | :1756, :1816-1820, :1977 |
| 2 | API version | 0x13, 0x15–0x1B sweep, all negative; official macro still 0x15 | ELIMINATED | :1757, :1789, :1977 |
| 3 | NvAPI init | `NvAPI_Initialize`=0 | ELIMINATED | :1759 |
| 4 | Wrapper / QI-vtable | Pristine clean device fails identically; QI(`IDXGIDevice`) failure is normal D3D12 behavior | FALSIFIED (write defused to log-only) | :1753-1755, :1827-1830 |
| 5 | Data-path writability | Models dir writable; dedicated fresh data subdir `models\ScaleNG_ngx` (run 163018Z) → identical codes | ELIMINATED | :1759, :1800-1802 |
| 6 | Classic-vs-Ext entry | Classic `0xBAD00001` vs Ext `0xBAD00002`, both fail; split explained as snippet answering each entry differently | ELIMINATED (explained) | :1758, :1811-1812, :1840-1842 |
| 7 | GPU identity | Game AND clean devices LUID-match 0x10DE RTX 3050 | ELIMINATED | :1760, :1790-1791 |
| 8 | Updater env | `__NGX_DISABLE_UPDATER` intentionally NOT set; removal changed nothing | ELIMINATED | :1761, `src/dlss_ngx.cpp:222-224` |
| 9 | Deny-list | Unrelated entry | ELIMINATED | :1761 |
| 10 | In-process interference | Standalone `tests/ngx_init_probe.cpp` on explicit NVIDIA device, out-of-game: identical `0xBAD00002` | ELIMINATED | :1762-1763 |
| 11 | AllocateParameters | Absent export; made load-optional, never called | ELIMINATED (load-optional) | :1752, :1649, :1827 |
| 12 | ProjectID route | No export in this DLL | UNAVAILABLE (no run) | :1792-1793 |
| 13 | Driver repair/reinstall | No evidence implicating the driver; driver known-stable/current by user selection | WITHDRAWN as unproven (was overstated standing hypothesis) | :1764-1768, :1813-1815 |
| 14 | SIMCLASS forum experiment | Code comment only (`src/dlss_ngx.cpp:220-221` cites run 163545Z identical `0xBAD00002`, REVERTED, no behavior remnant) | UNBACKED — no artifact; NOT eliminated | src/dlss_ngx.cpp:220-221 |
| 15 | D3D11 path | No mention found in investigated sources | UNBACKED — NOT eliminated | — |

Predecessor runs: `20261007T163018Z` (fresh data subdir, identical codes —
STATUS.md:1800-1802) and `20261007T163545Z` (SIMCLASS identical `0xBAD00002`
per code comment — `src/dlss_ngx.cpp:220-221`): all-fail, ~20x each
code, zero evals.

Resolution (STATUS.md:1821-1826, 1955-1961): reviewer hypothesis B,
filesystem-CONFIRMED — loader hardcoded only `nvlti.inf_*`; real core at
`nvltsi.inf_amd64_...\nvngx.dll` (489KB); census-enumerated loader
(`nv*.inf_*`) loads CORE first; Init_Ext + NGX logging then SUCCEED.

Scoreboard at boundary (STATUS.md:1976-1980): classic `0xBAD00001` × (AppId
×3, versions 0x13+0x15–0x1B, both devices, in+out of game); Ext `0xBAD00002`
× (same matrix + fresh dir). Every project-local input variant exhausted.

Open at era end (NOT findings): why snippet bring-up fails on a supported
adapter (STATUS.md:1797-1799); color correctness; MV/depth semantics;
visual benefit.

Return: [Current status](../../STATUS.md) · [Archive index](../README.md)
