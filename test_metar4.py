
import unittest
import metar4

# VFR > 3000ft & 5mi
# MVFR 1-3k and/or 3-5mi
# IFR 500 to <1000 and/or 1-3mi
# LIFR < 500 and/or < 1mi

class FlightCategoryTests(unittest.TestCase):
  def test_clouds(self):
    # any visibility 10+ is obviously not a threat, so just setting it to 99 so we can forget about it.
    self.assertEqual( metar4.flight_category(99, 3005), "VFR")
    self.assertEqual( metar4.flight_category(99, 3000), "VFR")
    self.assertEqual( metar4.flight_category(99, 2999), "MVFR")
    self.assertEqual( metar4.flight_category(99, 1005), "MVFR")
    self.assertEqual( metar4.flight_category(99, 1000), "MVFR")
    self.assertEqual( metar4.flight_category(99, 999), "IFR")
    self.assertEqual( metar4.flight_category(99, 505), "IFR")
    self.assertEqual( metar4.flight_category(99, 500), "IFR")
    self.assertEqual( metar4.flight_category(99, 499), "LIFR")
    self.assertEqual( metar4.flight_category(99, 1), "LIFR")

  def test_cloud_at_zero(self):
    # zero = fog
    self.assertEqual( metar4.flight_category(99, 0), "LIFR")

  def test_cloud_none(self):
    # none = CAVU
    self.assertEqual( metar4.flight_category(99, None), "VFR")

  def test_visibility(self):
    # any clouds >3000ft is obviously not a threat, so just setting it to 9999 so we can forget about it.
    self.assertEqual( metar4.flight_category(99, 9999), "VFR")
    self.assertEqual( metar4.flight_category(10, 9999), "VFR")
    self.assertEqual( metar4.flight_category(9, 9999), "VFR")
    self.assertEqual( metar4.flight_category(5.01, 9999), "VFR")
    self.assertEqual( metar4.flight_category(5, 9999), "VFR")
    self.assertEqual( metar4.flight_category(4.99, 9999), "MVFR")
    self.assertEqual( metar4.flight_category(3.01, 9999), "MVFR")
    self.assertEqual( metar4.flight_category(3, 9999), "MVFR")
    self.assertEqual( metar4.flight_category(2.99, 9999), "IFR")
    self.assertEqual( metar4.flight_category(1.01, 9999), "IFR")
    self.assertEqual( metar4.flight_category(1, 9999), "IFR")
    self.assertEqual( metar4.flight_category(0.99, 9999), "LIFR")
