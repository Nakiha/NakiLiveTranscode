"""Decoder private output/reconstruction overlap must join before public delivery."""
import importlib.util
import pathlib
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('codec_phase_audit',ROOT/'scripts/audit-codec-phases.py')
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
class DecoderJoin(unittest.TestCase):
    def event(self,selected=130000,worker=140000,queued=150000):
        return dict(decoder_submit_monotonic_us=10000,decoded_monotonic_us=180000,decoder_internal=dict(
            version=1,dispatch_us=20000,worker_start_us=30000,decode_begin_us=40000,picture_start_us=50000,
            reconstructed_us=100000,output_selected_us=selected,worker_output_us=worker,
            async_queued_us=queued,frontend_us=170000,decode_wall_us=60000,decode_cpu_us=40000,reference_wait_us=10000))
    def check_sum(self,event,result):
        keys=['async_admission','worker_launch','before_decode','decode_window','display_reorder',
              'worker_delivery','async_transfer','frontend_queue','app_handoff']
        self.assertEqual(sum(result[k] for k in keys),(event['decoded_monotonic_us']-event['decoder_submit_monotonic_us'])/1000)
        self.assertTrue(all(result[k]>=0 for k in keys))
    def test_serial_delivery(self):
        event=self.event();result=audit.decode_parts(event);self.check_sum(event,result)
        self.assertEqual(result['worker_delivery'],10);self.assertEqual(result['worker_output_lead'],0)
    def test_private_output_before_reconstruction(self):
        event=self.event(selected=70000,worker=80000,queued=110000);result=audit.decode_parts(event)
        self.check_sum(event,result);self.assertEqual(result['worker_output_lead'],20)
        self.assertEqual(result['worker_delivery'],0);self.assertEqual(result['async_transfer'],10)
    def test_visible_output_before_reconstruction_rejected(self):
        with self.assertRaises(ValueError):audit.decode_parts(self.event(selected=70000,worker=80000,queued=90000))
    def test_private_output_before_selection_rejected(self):
        with self.assertRaises(ValueError):audit.decode_parts(self.event(selected=90000,worker=80000))
    def test_missing_time_rejected(self):
        event=self.event();event['decoder_internal']['worker_output_us']=0
        with self.assertRaises(ValueError):audit.decode_parts(event)
class CpuWindows(unittest.TestCase):
    def test_v2_slewed_timeline_uses_raw_cpu_enclosure(self):
        event=DecoderJoin().event()
        event['decoder_internal'].update(version=2,decode_cpu_us=60024,decode_cpu_wall_us=60030)
        result=audit.decode_parts(event)
        self.assertEqual(result['cpu_monotonic_excess'],0.024)
        self.assertEqual(result['decode_cpu'],60.024)
        self.assertAlmostEqual(result['decode_off_cpu'],0.006)
    def test_v2_raw_overrun_still_fails(self):
        event=DecoderJoin().event()
        event['decoder_internal'].update(version=2,decode_cpu_us=60024,decode_cpu_wall_us=60000)
        with self.assertRaises(AssertionError):audit.decode_parts(event)
    def test_v1_historical_failure_is_not_regraded(self):
        event=DecoderJoin().event()
        event['decoder_internal']['decode_cpu_us']=60024
        with self.assertRaises(AssertionError):audit.decode_parts(event)
    def test_nested_cpu_window(self):
        audit.bounded_cpu_us(9,100,111)
    def test_observed_clock_order_error_rejected(self):
        with self.assertRaises(AssertionError):audit.bounded_cpu_us(31,100,111)
    def test_negative_cpu_rejected(self):
        with self.assertRaises(AssertionError):audit.bounded_cpu_us(-1,100,111)
    def test_observed_cross_clock_excess_is_reported_without_clamping_cpu(self):
        self.assertEqual(audit.bounded_cpu_us(329,100,426),3)
        self.assertEqual(audit.bounded_cpu_us(331,100,426),5)
        with self.assertRaises(AssertionError):audit.bounded_cpu_us(332,100,426)

if __name__=='__main__':unittest.main()
