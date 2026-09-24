def dedupe_records(rows):
    # Keep first spelling; compare stripped casefolded names.
    seen = []
    result = []
    for row in rows:
        key = row.strip().casefold()
        if key not in seen:
            seen.append(key)
            result.append(row)
    return result
