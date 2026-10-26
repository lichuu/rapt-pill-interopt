import re, os, subprocess
b = open('rapt-pill-2.0.0.bin','rb').read()
RO_S, RO_E, RO_ADDR = 0x20, 0x44908, 0x3f400020 - 0x20  # file range of DROM segment
A = lambda o: o + RO_ADDR

os.makedirs('extract/certs', exist_ok=True)
os.makedirs('extract/assets', exist_ok=True)

for i, m in enumerate(re.finditer(rb'-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----', b[RO_S:RO_E])):
    fn = 'extract/certs/ca%d_0x%08x.pem' % (i, A(RO_S + m.start()))
    open(fn, 'w').write(m.group().decode() + "\n")
    r = subprocess.run(['openssl', 'x509', '-in', fn, '-noout', '-subject', '-issuer',
                        '-enddate'], capture_output=True, text=True)
    print(fn, '->', (r.stdout or r.stderr).strip().replace('\n', ' | '))

# Embedded web assets: long runs of printable text with markup/code density
runs = []
for m in re.finditer(rb'[\x20-\x7e\n\r\t]{400,}', b[RO_S:RO_E]):
    s = m.group()
    score = sum(s.count(t) for t in (b'function ', b'<div', b'const ', b'=', b'{', b'}'))
    if score > 20:
        runs.append((RO_S + m.start(), m.end() + RO_S, s))
runs = [r for r in runs if not r[2].startswith(b'-----BEGIN')]
print('\ntext blobs >=400B with code/markup density:', len(runs))
for s, e, s_ in runs:
    head = s_[:60].decode('latin-1').replace('\n', ' ')
    kind = 'html' if b'<html' in s_[:200] else ('css' if s_[:40].count(b'{') and b'style' in s_[:200].lower() else 'js')
    fn = 'extract/assets/0x%08x.%s' % (A(s), kind)
    open(fn, 'w').write(s_.decode('latin-1'))
    print('  %-40s len=%-7d %s' % (os.path.basename(fn), e - s, head))
