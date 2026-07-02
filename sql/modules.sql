--
-- Session initialization via the plruby_modules table.
--
-- NOTE: modules are loaded once, on the first PL/Ruby use in a session.  Since
-- the extension is already initialized in this regression session, this test
-- verifies the table shape and documents the mechanism; the autoload itself is
-- exercised in a fresh session during manual testing.
--
CREATE TABLE plruby_modules (modname text, modseq int, modsrc text);
INSERT INTO plruby_modules VALUES
    ('util', 0, 'def slugify(s); s.strip.downcase.gsub(/\s+/, %q{-}); end');

-- Load the module explicitly for this already-initialized session by
-- evaluating it through a DO block, then call the helper it defines.
DO $$
    r = spi_exec("select modsrc from plruby_modules order by modname, modseq")
    while (row = spi_fetch_row(r)); eval(row['modsrc']); end
$$ LANGUAGE plruby;

CREATE FUNCTION use_slug(text) RETURNS text LANGUAGE plruby AS $$
    slugify(args[0])
$$;
SELECT use_slug('  Hello  World  ');

DROP TABLE plruby_modules;
