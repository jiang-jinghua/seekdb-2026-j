from pathlib import Path
import sys
import pyarrow as pa, pyarrow.parquet as pq
from datetime import date,datetime,timedelta
root=Path(sys.argv[1]);root.mkdir(parents=True,exist_ok=True)
rows=257
values={'id':pa.array([i if i%7 else None for i in range(rows)],type=pa.int64()),'score':pa.array([i/4 if i%5 else None for i in range(rows)],type=pa.float64()),'active':pa.array([i%2==0 if i%3 else None for i in range(rows)],type=pa.bool_()),'day':pa.array([date(2024,1,1)+timedelta(days=i) if i%4 else None for i in range(rows)],type=pa.date32()),'created':pa.array([datetime(2024,1,1)+timedelta(microseconds=i) if i%6 else None for i in range(rows)],type=pa.timestamp('us')),'text':pa.array(['文字'+str(i%3) if i%8 else None for i in range(rows)],type=pa.string())}
table=pa.table(values)
for version in ['1.0','2.0']:
 for codec in ['NONE','snappy']:
  for dictionary in [False,True]:
   path=root/f'v{version}-{codec}-{dictionary}.parquet'
   pq.write_table(table,path,data_page_version=version,compression=codec,use_dictionary=dictionary,row_group_size=31,data_page_size=256,write_batch_size=16)
print('created 8 nullable, multi-row-group Parquet variants')

pq.write_table(pa.table({"u32": pa.array([2**32-1], type=pa.uint32()), "u64": pa.array([2**64-1], type=pa.uint64())}), root/"unsigned.parquet", compression="snappy")
