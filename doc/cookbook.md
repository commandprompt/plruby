# PL/Ruby cookbook

Practical, working recipes that use Ruby's standard library where PostgreSQL
alone is awkward. Everything under **Tested recipes** is exercised verbatim by
the regression suite (`sql/cookbook.sql`), so the code is guaranteed to run on
every PostgreSQL version the extension supports.

Ruby's standard library is available via plain `require` — including the
parts that ship as bundled gems on newer Rubies (`csv`, `bigdecimal`, ...) —
which is what most of these lean on: `json`, `csv`, `openssl`, `zlib`,
`bigdecimal`, `erb`, `uri`. Gems you install alongside the server's Ruby are
requirable too.

## Tested recipes

### Validate an email address

`URI::MailTo::EMAIL_REGEXP` is a maintained, RFC-conscious pattern that ships
with Ruby — no extension packages needed.

```sql
CREATE FUNCTION is_email(text) RETURNS boolean LANGUAGE plruby AS $$
    require 'uri'
    args[0].to_s.match?(URI::MailTo::EMAIL_REGEXP)
$$;

SELECT is_email('user@example.com');   -- t
ALTER TABLE users ADD CONSTRAINT valid_email CHECK (is_email(email));
```

### Redact sensitive keys anywhere in a JSON document

`jsonb` arrives as a String; `JSON.parse` turns it into Ruby data, and a
recursive lambda rewrites it at any depth. Note the `VARIADIC` key list.

```sql
CREATE FUNCTION jsonb_redact(doc jsonb, VARIADIC keys text[]) RETURNS jsonb
LANGUAGE plruby AS $$
    require 'json'
    redact = lambda { |v|
        case v
        when Hash  then v.to_h { |k, e| [k, keys.include?(k) ? '[redacted]' : redact.call(e)] }
        when Array then v.map { |e| redact.call(e) }
        else v
        end
    }
    JSON.generate(redact.call(JSON.parse(doc)))
$$;

SELECT jsonb_redact(payload, 'password', 'token') FROM api_requests;
```

### Extract every match of a pattern, one row each

`String#scan` plus `return_next` makes a set-returning regex extractor.

```sql
CREATE FUNCTION extract_emails(text) RETURNS SETOF text LANGUAGE plruby AS $$
    args[0].scan(/[\w+.-]+@[\w.-]+\.\w+/) { |m| return_next m }
    nil
$$;

SELECT * FROM extract_emails('Contact ann@example.com or bob@test.org today.');
```

### HMAC-signed tokens

Sign a value so a client cannot tamper with it, and verify it in constant
time on the way back in. `Array#pack('m0')` handles the Base64 leg — no
extra requires (the `base64` library left Ruby's default gems in 3.4).

```sql
CREATE FUNCTION sign_token(payload text, key text) RETURNS text LANGUAGE plruby AS $$
    require 'openssl'
    mac = OpenSSL::HMAC.hexdigest('SHA256', args[1], args[0])
    "#{[args[0]].pack('m0')}.#{mac}"
$$;

CREATE FUNCTION verify_token(token text, key text) RETURNS text LANGUAGE plruby AS $$
    require 'openssl'
    data, mac = args[0].split('.', 2)
    payload = data.to_s.unpack1('m0') rescue nil
    if payload
        expected = OpenSSL::HMAC.hexdigest('SHA256', args[1], payload)
        OpenSSL.secure_compare(mac.to_s, expected) ? payload : nil
    end
$$;
```

`verify_token` returns the payload when the signature is valid and `NULL`
otherwise.

### Password hashing without pgcrypto

PBKDF2-HMAC-SHA256 via `OpenSSL::KDF`, with the parameters stored alongside
the digest. Generate the salt with `SecureRandom.hex(16)` in practice; it is
a parameter here so the recipe is testable.

```sql
CREATE FUNCTION hash_password(pw text, salt text) RETURNS text LANGUAGE plruby AS $$
    require 'openssl'
    digest = OpenSSL::KDF.pbkdf2_hmac(args[0], salt: args[1],
                                      iterations: 20_000, length: 32,
                                      hash: 'SHA256')
    "pbkdf2$20000$#{args[1]}$#{digest.unpack1('H*')}"
$$;

CREATE FUNCTION check_password(pw text, stored text) RETURNS boolean LANGUAGE plruby AS $$
    require 'openssl'
    _scheme, iters, salt, hex = args[1].split('$', 4)
    digest = OpenSSL::KDF.pbkdf2_hmac(args[0], salt: salt,
                                      iterations: iters.to_i, length: 32,
                                      hash: 'SHA256')
    OpenSSL.secure_compare(digest.unpack1('H*'), hex.to_s)
$$;
```

### A generic audit trigger

One trigger function for any table: it records the operation and the old/new
rows as `jsonb`. `$_TD` carries everything it needs.

```sql
CREATE TABLE audit_log (
    at_table text,
    op       text,
    old_row  jsonb,
    new_row  jsonb
);

CREATE FUNCTION audit() RETURNS trigger LANGUAGE plruby AS $$
    require 'json'
    old_j = $_TD['old'] ? quote_literal(JSON.generate($_TD['old'])) : 'null'
    new_j = $_TD['new'] ? quote_literal(JSON.generate($_TD['new'])) : 'null'
    spi_exec("insert into audit_log values (" +
             "#{quote_literal($_TD['relname'])}, #{quote_literal($_TD['event'])}, " +
             "#{old_j}, #{new_j})")
    nil
$$;

CREATE TRIGGER accounts_audit AFTER INSERT OR UPDATE OR DELETE ON accounts
    FOR EACH ROW EXECUTE PROCEDURE audit();
```

### Batch processing with periodic commits

A procedure (called with `CALL`) may commit as it goes, keeping locks short
and WAL bounded while it works through a large backlog.

```sql
CREATE PROCEDURE process_queue(batch int) LANGUAGE plruby AS $$
    loop do
        r = spi_exec("update cb_queue set done = true where id in " +
                     "(select id from cb_queue where not done limit #{args[0].to_i})")
        spi_commit
        break if spi_processed(r) == 0
    end
$$;

CALL process_queue(1000);
```

### Stream a big scan and stop early

`spi_query` reads through a cursor a batch at a time — constant memory no
matter how large the table — and `break` abandons the scan as soon as the
answer is known. Here: the first gap in an id sequence.

```sql
CREATE FUNCTION first_gap(tbl text) RETURNS int LANGUAGE plruby AS $$
    expected = 1
    spi_query("select id from #{quote_ident(args[0])} order by id") do |row|
        break if row['id'] != expected
        expected += 1
    end
    expected
$$;
```

### Parse CSV text into rows

Ruby's stdlib `csv` handles quoting, embedded commas and headers correctly —
things a hand-rolled `split(',')` gets wrong.

```sql
CREATE FUNCTION csv_rows(text) RETURNS TABLE (name text, qty int) LANGUAGE plruby AS $$
    require 'csv'
    CSV.parse(args[0], headers: true).each do |rec|
        return_next({'name' => rec['name'], 'qty' => rec['qty'].to_i})
    end
    nil
$$;

SELECT * FROM csv_rows(E'name,qty\nwidget,7\nsprocket,12\n');
```

### Compress large text into bytea and back

`bytea` travels as its hex text form (`\x...`), so pack/unpack bridges it to
raw bytes for `Zlib`.

```sql
CREATE FUNCTION gz(text) RETURNS bytea LANGUAGE plruby AS $$
    require 'zlib'
    "\\x" + Zlib::Deflate.deflate(args[0], Zlib::BEST_COMPRESSION).unpack1('H*')
$$;

CREATE FUNCTION gunz(bytea) RETURNS text LANGUAGE plruby AS $$
    require 'zlib'
    Zlib::Inflate.inflate([args[0].delete_prefix('\x')].pack('H*'))
$$;
```

### Exact decimal arithmetic with BigDecimal

`numeric` reaches Ruby as a lossless String precisely so it can feed
`BigDecimal` — no Float rounding anywhere.

```sql
CREATE FUNCTION money_share(total numeric, parts int) RETURNS numeric LANGUAGE plruby AS $$
    require 'bigdecimal'
    (BigDecimal(args[0]) / args[1]).round(2, BigDecimal::ROUND_HALF_EVEN).to_s('F')
$$;

SELECT money_share('100.00', 3);   -- 33.33
```

### Slugify a title (Unicode-aware)

NFKD decomposition splits letters from their accents; transcoding to ASCII
then drops the accents.

```sql
CREATE FUNCTION slugify(text) RETURNS text LANGUAGE plruby AS $$
    args[0].unicode_normalize(:nfkd)
           .encode('ASCII', invalid: :replace, undef: :replace, replace: '')
           .downcase.gsub(/[^a-z0-9]+/, '-').gsub(/\A-|-\z/, '')
$$;

SELECT slugify('Crème Brûlée à la Mode!');   -- creme-brulee-a-la-mode
```

### Render a report with ERB

Ruby's templating engine, in the database, fed from row data.

```sql
CREATE FUNCTION order_summary(cust text, total numeric) RETURNS text LANGUAGE plruby AS $$
    require 'erb'
    template = "Dear <%= cust %>, your order total is $<%= total %>."
    ERB.new(template).result_with_hash(cust: args[0], total: args[1])
$$;
```

## Doc-only recipes (side effects — use with care)

These work, but reach outside the database, so they are not part of the
regression suite. Remember that PL/Ruby is untrusted: functions run with the
server's OS-level privileges, and a slow network call happens inside your
transaction (do such work from an `AFTER` trigger, or better, enqueue it and
let a worker do the talking).

### HTTP webhook from a trigger

```sql
CREATE FUNCTION notify_webhook() RETURNS trigger LANGUAGE plruby AS $$
    require 'net/http'
    require 'json'
    uri = URI('https://hooks.example.com/order-created')
    Net::HTTP.post(uri, JSON.generate($_TD['new']),
                   'Content-Type' => 'application/json')
    nil
$$;
```

### Email notification

```sql
CREATE FUNCTION mail_alert(subject text, body text) RETURNS void LANGUAGE plruby AS $$
    require 'net/smtp'
    msg = "Subject: #{args[0]}\n\n#{args[1]}"
    Net::SMTP.start('localhost', 25) do |smtp|
        smtp.send_message(msg, 'db@example.com', 'ops@example.com')
    end
$$;
```

### Random tokens and UUIDs

```sql
CREATE FUNCTION api_key() RETURNS text LANGUAGE plruby AS $$
    require 'securerandom'
    SecureRandom.urlsafe_base64(32)
$$;
```

(`SecureRandom.uuid` likewise generates v4 UUIDs on PostgreSQL versions
without `gen_random_uuid()`.)

See [`doc/plruby.md`](plruby.md) for the full language reference.
