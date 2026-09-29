import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'app/deploy/linux/native-controller'))
from guide_taps import GuideTaps


def report(pressed=False, seq=10, extra=1):
    b=bytearray(46);b[0]=0x45;b[1]=seq
    b[2:6]=(extra | (GuideTaps.MASK if pressed else 0)).to_bytes(4,'little')
    b[10:12]=b'\x12\x34'
    return bytes(b)


def down(b):return bool(int.from_bytes(b[2:6],'little') & GuideTaps.MASK)


class GuideTests(unittest.TestCase):
    def test_single(self):
        g=GuideTaps();b,a=g.receive(report(True),0)
        self.assertFalse(down(b));self.assertIsNone(a)
        b,a=g.receive(report(False),.05);self.assertFalse(down(b))
        self.assertEqual(g.tick(.299),(None,None))
        b,a=g.tick(.301);self.assertTrue(down(b));self.assertEqual(a,'host')
        b,a=g.tick(.382);self.assertFalse(down(b));self.assertIsNone(a)
    def test_double(self):
        g=GuideTaps();g.receive(report(True),0);g.receive(report(False),.04)
        g.receive(report(True),.16);b,a=g.receive(report(False),.20)
        self.assertEqual(a,'local');self.assertFalse(down(b));self.assertEqual(g.tick(1),(None,None))
    def test_repeats_and_other_controls(self):
        g=GuideTaps()
        for i in range(100):
            b,a=g.receive(report(True,i,0x1234),i/1000)
            self.assertIsNone(a);self.assertEqual(int.from_bytes(b[2:6],'little'),0x1234)
            self.assertEqual(b[10:12],b'\x12\x34')
        self.assertIsNone(g.pending)
    def test_cancel(self):
        g=GuideTaps();g.receive(report(True),0);g.receive(report(False),.02);g.cancel()
        self.assertEqual(g.tick(1),(None,None))
    def test_non_state_reports(self):
        g=GuideTaps()
        for b in [b'', b'\x43\x10',bytes([0x45])*45,bytes([0x42])*46]:
            self.assertEqual(g.receive(b,0),(b,None))
    def test_sequence_and_release_on_idle(self):
        g=GuideTaps();a,_=g.receive(report(True,254),0);b,_=g.receive(report(False,255),.02)
        c,_=g.tick(.28);d,_=g.tick(.37)
        self.assertEqual([x[1] for x in [a,b,c,d]],[254,255,0,1]);self.assertFalse(down(d))
    def test_separated_taps(self):
        g=GuideTaps();g.receive(report(True),0);g.receive(report(False),.01);self.assertEqual(g.tick(.27)[1],'host');g.tick(.36)
        g.receive(report(True),.5);g.receive(report(False),.6);self.assertEqual(g.tick(.86)[1],'host')

if __name__=='__main__':unittest.main()
