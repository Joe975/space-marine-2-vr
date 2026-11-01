import csv
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class ThreadCpuTests(unittest.TestCase):
    def test_deltas_sort_and_missing_threads(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'before.csv').write_text('tid,description,total_ms\n2,render,100\n1,simulation,40\n3,gone,50\n4,reused,100\n')
            (root/'after.csv').write_text('tid,description,total_ms\n2,render,300\n1,simulation,940\n4,reused,20\n5,new,2000\n')
            command=[shutil.which('pwsh') or shutil.which('powershell'),'-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'scripts/sample-thread-cpu.ps1'),'-BeforeCsv',str(root/'before.csv'),'-AfterCsv',str(root/'after.csv'),'-ElapsedSeconds','1','-OutputCsv',str(root/'out.csv')]
            result=subprocess.run(command,capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            with (root/'out.csv').open(encoding='utf-8-sig') as file: rows=list(csv.DictReader(file))
            self.assertEqual([int(row['tid']) for row in rows],[1,2])
            self.assertEqual([float(row['percent_of_one_core']) for row in rows],[90,20])
            self.assertEqual(rows[0]['description'],'simulation')

if __name__=='__main__':unittest.main()
