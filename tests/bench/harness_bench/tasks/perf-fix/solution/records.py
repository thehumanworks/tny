def dedupe_records(rows):
    # Keep first spelling; compare stripped casefolded names.
    seen = set()
    result = []
    for row in rows:
        key = row.strip().casefold()
        if key not in seen:
            seen.add(key)
            result.append(row)
    return result
