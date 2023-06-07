## 1.0 2023-06-07

- Add support for array as header value (#2)

## 0.6.1 2023-05-28

- Fix sending response with frozen headers hash

## 0.6 2023-01-05

- Add documentation
- Implement `H1P.send_body_chunk`
- Implement `H1P.send_chunked_response`
- Implement `H1P.send_response`

## 0.5 2022-03-19

- Implement `Parser#splice_body_to` (#3)

## 0.4 2022-02-28

- Rename `__parser_read_method__` to `__read_method__`

## 0.3 2022-02-03

- Add support for parsing HTTP responses (#1)
- Put state directly in parser struct

## 0.2 2021-08-20

- Use uppercase for HTTP method

## 0.1 2021-08-19

- Import code from Tipi
