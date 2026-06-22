// Normalize machine.profile() JSON into a stable per-model SHAPE string, so a
// snapshot diff fails loudly when a field's *shape* changes (added, removed, or
// retyped) without breaking on value churn (RAM sizes, monitor counts, etc.) —
// proposal §6.1 "schema snapshot per model".
//
// Reads the headless dump on stdin (one JSON object per line, each carrying
// "id":"<model>"); writes "<id>\t<shape>" lines sorted by id to stdout.
//
// shape():
//   object  -> {k1:<shape>,k2:<shape>,...}   (keys sorted)
//   array   -> [<union of element shapes>]    (count-independent; [] when empty)
//   scalar  -> "string" | "number" | "boolean" | "null"

function shape(v) {
  if (v === null) return 'null';
  if (Array.isArray(v)) {
    if (v.length === 0) return '[]';
    const elems = [...new Set(v.map(shape))].sort();
    return '[' + elems.join('|') + ']';
  }
  if (typeof v === 'object') {
    const keys = Object.keys(v).sort();
    return '{' + keys.map((k) => k + ':' + shape(v[k])).join(',') + '}';
  }
  return typeof v; // string | number | boolean
}

let input = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (d) => (input += d));
process.stdin.on('end', () => {
  const rows = [];
  for (const line of input.split('\n')) {
    const t = line.trim();
    if (!t.startsWith('{')) continue;
    let obj;
    try {
      obj = JSON.parse(t);
    } catch {
      continue;
    }
    if (typeof obj.id !== 'string') continue;
    rows.push(obj.id + '\t' + shape(obj));
  }
  rows.sort();
  process.stdout.write(rows.join('\n') + '\n');
});
