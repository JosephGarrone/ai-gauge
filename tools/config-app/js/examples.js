/*
 * Starting points: the faces that ship on the device's storage partition. The Pages workflow copies
 * firmware/assets/gauges/*.xml to examples/, and the dev server maps the same path, so the editor
 * always offers exactly what the firmware ships. test/examples.test.mjs keeps this list complete.
 */

export const EXAMPLES = [
    { file: 'boost.xml', title: 'Boost, 0–30 psi' },
    { file: 'egt.xml', title: 'EGT, 0–900 °C' },
    { file: 'boost_custom.xml', title: 'Boost with custom ticks, needle and hub' },
];

/** A near-empty face: just enough to be valid. */
export const BLANK_XML = `<gauge version="1" id="new_face">
  <panel shape="round" width="466" height="466"/>
  <source channel="boost" unit="" min="0" max="100"/>
  <face start-angle="225" sweep="270" background="#000000">
    <ticks major-every="10" minor-every="0" color="#ffffff"/>
    <labels every="20" font="montserrat_24" color="#ffffff"/>
  </face>
  <needle color="#ff1744"/>
  <readout y="300" font="montserrat_48" format="%.0f" color="#ffffff"/>
</gauge>
`;
