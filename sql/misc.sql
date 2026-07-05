--
-- Additional scalar types with no dedicated Ruby representation: uuid, inet,
-- enums, and domains.  All travel as strings; constraints and input
-- validation stay on the PostgreSQL side.
--

-- uuid round-trips as its canonical lower-case text form.
CREATE FUNCTION u_echo(uuid) RETURNS uuid LANGUAGE plruby AS $$
    args[0]
$$;
SELECT u_echo('A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11');

CREATE FUNCTION u_class(uuid) RETURNS text LANGUAGE plruby AS $$
    args[0].class.name
$$;
SELECT u_class('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11');

-- inet/cidr as strings; the network math stays in SQL.
CREATE FUNCTION net_echo(inet) RETURNS inet LANGUAGE plruby AS $$
    args[0]
$$;
SELECT net_echo('192.168.100.128/25');
SELECT net_echo('::1');

-- An enum arrives as its label and may be returned as one.
CREATE TYPE mt_mood AS ENUM ('sad', 'ok', 'happy');
CREATE FUNCTION mood_up(mt_mood) RETURNS mt_mood LANGUAGE plruby AS $$
    order_ = ['sad', 'ok', 'happy']
    order_[[order_.index(args[0]) + 1, order_.length - 1].min]
$$;
SELECT mood_up('sad'), mood_up('ok'), mood_up('happy');

-- Returning a label that is not part of the enum is rejected.
CREATE FUNCTION mood_bad() RETURNS mt_mood LANGUAGE plruby AS $$
    'ecstatic'
$$;
SELECT mood_bad();

-- Domains: values returned into a domain are checked by its constraint.
CREATE DOMAIN mt_posint AS int CHECK (VALUE > 0);
CREATE FUNCTION pos_ok(int) RETURNS mt_posint LANGUAGE plruby AS $$
    args[0]
$$;
SELECT pos_ok(5);
SELECT pos_ok(-5);

-- A domain-typed argument arrives as a String (its text form), not as the
-- base type's Ruby value -- the conversion goes by the argument's own type.
CREATE FUNCTION pos_class(mt_posint) RETURNS text LANGUAGE plruby AS $$
    "#{args[0].class.name}=#{args[0]}"
$$;
SELECT pos_class(41);

DROP FUNCTION pos_class(mt_posint);
DROP FUNCTION pos_ok(int);
DROP DOMAIN mt_posint;
DROP FUNCTION mood_up(mt_mood);
DROP FUNCTION mood_bad();
DROP TYPE mt_mood;
