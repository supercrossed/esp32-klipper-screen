# Li-ion battery curve

The Li-ion voltage-to-% table in `main/battery.c` (`OCV_LIION`) is the average of four measured low-current discharges. The board draws only a few hundred mA, so slow discharges like these match how the cell is used.

| Cell | Test | Source |
|---|---|---|
| LG 18650HG2, 3.0 Ah (NMC) | C/20 discharge, 150 mA, 25 °C | Kollmeyer et al., McMaster University: [LG 18650HG2 Li-ion Battery Data](https://data.mendeley.com/datasets/b5mj79w5w9/2), file `25degC/549_C20DisCh.csv` |
| Samsung INR21700-30T, 3.0 Ah (NMC) | C/20 discharge, 150 mA, 25 °C | Kollmeyer et al.: [Samsung INR21700 30T 3Ah Li-ion Battery Data](https://data.mendeley.com/datasets/9xyvy2njj3/2), file `25degC/729_C20DisCh.csv` |
| Panasonic NCR18650PF, 2.9 Ah (NCA) | C/20 discharge, 145 mA, 25 °C | Kollmeyer, University of Wisconsin-Madison: [Panasonic 18650PF Li-ion Battery Data](https://data.mendeley.com/datasets/wykht8y7tg/1), file `25degC/C20 OCV and 1C discharge tests_start_of_tests/05-08-17_13.26 C20 OCV Test_C20_25dC.mat` |
| Samsung INR18650-20R, 2.0 Ah (NMC) | 0.05C low-current OCV test, 100 mA, 25 °C | [CALCE Battery Research Group](https://calce.umd.edu/data), University of Maryland: `SP1_25C_LC_OCV_11_5_2015.zip` |

Each cell's % is measured against its own capacity between full and 3.0 V, so large and small cells count equally. The four agree within about 30–75 mV between 15% and 50%. They spread more near full (NCA and the older 20R sit lower) and below 10%.

Result (V at % left):

| % | 100 | 90 | 80 | 70 | 60 | 50 | 40 | 30 | 20 | 10 | 5 | 0 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| V | 4.18 | 4.06 | 3.98 | 3.88 | 3.79 | 3.69 | 3.62 | 3.57 | 3.49 | 3.38 | 3.30 | 3.00 |

The firmware then adjusts for the board's actual load, and makes 0% the "Turn off when empty" voltage.

## Rebuild

Download the files above into `cells/` next to the scripts and convert them:

- `hg2_c20.csv` and `s30t_c20.csv`: the CSVs as downloaded.
- `pf_dis.csv`: the discharge rows of the `.mat` file as `voltage, current, Ah from start`, made with scipy.
- `sp20r_dis.csv`: program step 3 of the CALCE spreadsheet as `voltage, current, Ah` (Ah integrated from mA and time).

Then:

```
python aggregate.py
```

It prints each cell's curve, the average, and the C table.

The datasets are not included in this repository. The Mendeley datasets are CC BY 4.0. CALCE asks that any publication using its data cite the CALCE papers that describe the experiments.
