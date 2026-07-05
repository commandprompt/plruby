--
-- The tested cookbook recipes (doc/cookbook.md).  Each function here appears
-- verbatim in the cookbook; this test keeps the recipes honest.
--
SET bytea_output = 'hex';

-- Recipe: validate an email address with Ruby's stdlib.
CREATE FUNCTION is_email(text) RETURNS boolean LANGUAGE plruby AS $$
    require 'uri'
    args[0].to_s.match?(URI::MailTo::EMAIL_REGEXP)
$$;
SELECT is_email('user@example.com') AS good, is_email('not-an-email') AS bad;

-- Recipe: redact sensitive keys anywhere in a JSON document.
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
SELECT jsonb_redact(
    '{"user": "jd", "password": "s3cret", "sessions": [{"token": "abc", "ip": "10.0.0.1"}]}',
    'password', 'token');

-- Recipe: extract every match of a pattern, one row each.
CREATE FUNCTION extract_emails(text) RETURNS SETOF text LANGUAGE plruby AS $$
    args[0].scan(/[\w+.-]+@[\w.-]+\.\w+/) { |m| return_next m }
    nil
$$;
SELECT * FROM extract_emails('Contact ann@example.com or bob@test.org today.');

-- Recipe: HMAC-signed tokens (sign a value so clients cannot tamper with it).
CREATE FUNCTION sign_token(payload text, key text) RETURNS text LANGUAGE plruby AS $$
    require 'openssl'
    require 'base64'
    mac = OpenSSL::HMAC.hexdigest('SHA256', args[1], args[0])
    "#{Base64.strict_encode64(args[0])}.#{mac}"
$$;
CREATE FUNCTION verify_token(token text, key text) RETURNS text LANGUAGE plruby AS $$
    require 'openssl'
    require 'base64'
    data, mac = args[0].split('.', 2)
    payload = Base64.strict_decode64(data.to_s)
    expected = OpenSSL::HMAC.hexdigest('SHA256', args[1], payload)
    OpenSSL.secure_compare(mac.to_s, expected) ? payload : nil
$$;
SELECT sign_token('user=42', 'server-key');
SELECT verify_token(sign_token('user=42', 'server-key'), 'server-key') AS valid,
       verify_token('dXNlcj00Mg==.forged', 'server-key') IS NULL AS forged_rejected;

-- Recipe: PBKDF2 password hashing without pgcrypto.
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
SELECT check_password('hunter2', hash_password('hunter2', 'fixed-salt')) AS ok,
       check_password('wrong', hash_password('hunter2', 'fixed-salt')) AS bad;

-- Recipe: a generic audit trigger usable on any table.
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
CREATE TABLE cb_accounts (id int PRIMARY KEY, balance int);
CREATE TRIGGER cb_accounts_audit AFTER INSERT OR UPDATE OR DELETE ON cb_accounts
    FOR EACH ROW EXECUTE PROCEDURE audit();
INSERT INTO cb_accounts VALUES (1, 100);
UPDATE cb_accounts SET balance = 150 WHERE id = 1;
DELETE FROM cb_accounts WHERE id = 1;
SELECT at_table, op, old_row, new_row FROM audit_log;

-- Recipe: batch processing with periodic commits (bounded WAL/locks).
CREATE TABLE cb_queue AS SELECT g AS id, false AS done FROM generate_series(1, 10) g;
CREATE PROCEDURE process_queue(batch int) LANGUAGE plruby AS $$
    loop do
        r = spi_exec("update cb_queue set done = true where id in " +
                     "(select id from cb_queue where not done limit #{args[0].to_i})")
        spi_commit
        break if spi_processed(r) == 0
    end
$$;
CALL process_queue(4);
SELECT count(*) FILTER (WHERE done) AS done, count(*) AS total FROM cb_queue;

-- Recipe: stream a large scan and stop early (constant memory, no OFFSET).
CREATE FUNCTION first_gap(tbl text) RETURNS int LANGUAGE plruby AS $$
    expected = 1
    spi_query("select id from #{quote_ident(args[0])} order by id") do |row|
        break if row['id'] != expected
        expected += 1
    end
    expected
$$;
CREATE TABLE cb_ids AS SELECT g AS id FROM generate_series(1, 1000) g;
DELETE FROM cb_ids WHERE id = 7;
SELECT first_gap('cb_ids');

-- Recipe: parse CSV text into rows.
CREATE FUNCTION csv_rows(text) RETURNS TABLE (name text, qty int) LANGUAGE plruby AS $$
    require 'csv'
    CSV.parse(args[0], headers: true).each do |rec|
        return_next({'name' => rec['name'], 'qty' => rec['qty'].to_i})
    end
    nil
$$;
SELECT * FROM csv_rows(E'name,qty\nwidget,7\nsprocket,12\n');

-- Recipe: compress large text into bytea and back.
CREATE FUNCTION gz(text) RETURNS bytea LANGUAGE plruby AS $$
    require 'zlib'
    "\\x" + Zlib::Deflate.deflate(args[0], Zlib::BEST_COMPRESSION).unpack1('H*')
$$;
CREATE FUNCTION gunz(bytea) RETURNS text LANGUAGE plruby AS $$
    require 'zlib'
    Zlib::Inflate.inflate([args[0].delete_prefix('\x')].pack('H*'))
$$;
SELECT gunz(gz(repeat('the same phrase over and over ', 1000)))
         = repeat('the same phrase over and over ', 1000) AS roundtrip,
       octet_length(gz(repeat('the same phrase over and over ', 1000))) < 500 AS small;

-- Recipe: exact decimal arithmetic with BigDecimal (numeric is a String).
CREATE FUNCTION money_share(total numeric, parts int) RETURNS numeric LANGUAGE plruby AS $$
    require 'bigdecimal'
    (BigDecimal(args[0]) / args[1]).round(2, BigDecimal::ROUND_HALF_EVEN).to_s('F')
$$;
SELECT money_share('100.00', 3);

-- Recipe: slugify a title (Unicode-aware).
CREATE FUNCTION slugify(text) RETURNS text LANGUAGE plruby AS $$
    args[0].unicode_normalize(:nfkd)
           .encode('ASCII', invalid: :replace, undef: :replace, replace: '')
           .downcase.gsub(/[^a-z0-9]+/, '-').gsub(/\A-|-\z/, '')
$$;
SELECT slugify(U&'Cr\00e8me Br\00fbl\00e9e \00e0 la Mode!');

-- Recipe: render a report with ERB.
CREATE FUNCTION order_summary(cust text, total numeric) RETURNS text LANGUAGE plruby AS $$
    require 'erb'
    template = "Dear <%= cust %>, your order total is $<%= total %>."
    ERB.new(template).result_with_hash(cust: args[0], total: args[1])
$$;
SELECT order_summary('Ada', '99.95');

DROP TABLE cb_ids;
DROP TABLE cb_queue;
DROP TABLE cb_accounts;
DROP TABLE audit_log;
