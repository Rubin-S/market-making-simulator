"""Cut the two Tardis heads to a common exchange-time window and convert with hftbacktest's tardis.convert.
Usage: python3 convert_tardis.py [minutes]   (default: the whole common window)
Writes data/tardis_btcusdt_slice.npz plus the cut CSVs actually fed to the converter (the C++ engine must
read exactly these two CSVs: data/cut_trades.csv and data/cut_incremental_book_L2.csv)."""
import sys, numpy as np, polars as pl
from hftbacktest.data.utils import tardis
book = pl.read_csv('data/tardis_incremental_book_L2_head.csv')
trd = pl.read_csv('data/tardis_trades_head.csv')
# Tardis timestamps are microseconds since epoch (UTC)
t_end = min(book['timestamp'].max(), trd['timestamp'].max())
if len(sys.argv) > 1:
    t0 = book.filter(~pl.col('is_snapshot'))['timestamp'].min()
    t_end = min(t_end, t0 + int(float(sys.argv[1]) * 60e6))
# drop a possibly incomplete last message: keep rows strictly before the last timestamp in the window
book = book.filter(pl.col('timestamp') < t_end)
trd = trd.filter(pl.col('timestamp') < t_end)
book.write_csv('data/cut_incremental_book_L2.csv'); trd.write_csv('data/cut_trades.csv')
print('window us', book['timestamp'].min(), t_end, 'minutes', (t_end - book['timestamp'].min()) / 60e6,
      'book rows', len(book), 'snapshot rows', int(book['is_snapshot'].sum()), 'trade rows', len(trd))
data = tardis.convert(['data/cut_trades.csv', 'data/cut_incremental_book_L2.csv'],
                      output_filename='data/tardis_btcusdt_slice.npz', buffer_size=len(book) + len(trd) + 10,
                      snapshot_mode='process')
print('events', len(data))
