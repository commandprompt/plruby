--
-- $stdout / $stderr are redirected to the PostgreSQL log, one LOG line per
-- completed output line.  Raise client_min_messages so the driver sees them.
--
SET client_min_messages = LOG;

CREATE FUNCTION io_puts() RETURNS void LANGUAGE plruby AS $$
    puts 'line via puts'
    $stdout.write('partial ')
    $stdout.write("then newline\n")
    $stderr.puts 'line via stderr'
    nil
$$;
SELECT io_puts();

-- printf and << go through the same redirection.
CREATE FUNCTION io_printf() RETURNS void LANGUAGE plruby AS $$
    printf("formatted %d-%s\n", 42, 'x')
    $stdout << "appended\n"
    nil
$$;
SELECT io_printf();

RESET client_min_messages;
