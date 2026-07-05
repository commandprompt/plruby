--
-- String encoding: text from the database is tagged with the Ruby encoding
-- matching the database encoding, so multibyte operations work by character.
-- This regression database is UTF-8; other encodings map analogously
-- (LATIN1 -> ISO-8859-1, EUC_JP -> EUC-JP, WIN1251 -> Windows-1251, ...),
-- with unmapped encodings falling back to ASCII-8BIT.
--
CREATE FUNCTION enc_name(text) RETURNS text LANGUAGE plruby AS $$
    args[0].encoding.name
$$;
SELECT enc_name('x') AS db_encoding;

-- A multibyte value arrives tagged, so length/reverse operate on characters.
CREATE FUNCTION enc_mb(text) RETURNS text LANGUAGE plruby AS $$
    "chars=#{args[0].length} bytes=#{args[0].bytesize} " +
        "enc=#{args[0].encoding.name} rev=#{args[0].reverse}"
$$;
SELECT enc_mb(U&'caf\00e9');
