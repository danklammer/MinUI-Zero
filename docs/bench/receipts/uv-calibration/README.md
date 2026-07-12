# Undervolt calibration receipt

`margins.log` is the preserved on-device campaign log. `table.pre-audit.conf` is the
table emitted by that campaign's original generator.

`table.conf`, `table.stock`, and `calibration` are deterministic derived outputs from the same log using
the v1.3 release generator. The corrected generator caps every row at its measured stock
voltage and publishes a non-decreasing ceiling envelope. This changes the low rows from
812.5 mV to their recorded 762.5 mV stock value and raises the 1800 MHz row to 1075 mV so
it covers the higher 1608 MHz requirement below that ceiling.

The raw minimum cliff headroom is 75 mV at 1608 MHz. The production table keeps a 50 mV
guard above that cliff, so its minimum applied reduction is 25 mV. At 1800 MHz, the
production reduction is 112.5 mV (stored as integer `top_reduction_mv=112`). Device serial
and model receipts are intentionally redacted.
