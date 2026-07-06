--
-- The Ruby standard library is reachable via require.  RubyGems is enabled
-- so that libraries shipped as bundled gems on newer Rubies (csv,
-- bigdecimal, base64, ...) resolve too.
--
CREATE FUNCTION r_stdlib() RETURNS text LANGUAGE plruby AS $$
    %w[json date set digest securerandom].map { |m|
        begin
            require m
            "#{m}=ok"
        rescue LoadError
            "#{m}=FAIL"
        end
    }.join(' ')
$$;
SELECT r_stdlib();

-- A required library actually works: JSON generation and a SHA-256 digest.
CREATE FUNCTION r_json() RETURNS text LANGUAGE plruby AS $$
    require 'json'
    JSON.generate({'a' => 1, 'b' => [2, 3]})
$$;
SELECT r_json();

CREATE FUNCTION r_digest() RETURNS text LANGUAGE plruby AS $$
    require 'digest'
    Digest::SHA256.hexdigest('plruby')
$$;
SELECT r_digest();
