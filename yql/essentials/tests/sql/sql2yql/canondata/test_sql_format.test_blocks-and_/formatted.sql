$data = [
    <|x: FALSE, y: FALSE|>,
    <|x: FALSE, y: TRUE|>,
    <|x: TRUE, y: FALSE|>,
    <|x: TRUE, y: TRUE|>,
];

SELECT
    x AND y
FROM
    as_table($data)
;
