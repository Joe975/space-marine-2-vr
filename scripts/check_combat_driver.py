"""Check combat_ownership's actual native pad trace; native animations are a separate gate."""
import argparse
import json
from pathlib import Path


def check(records, request_id):
    marks = [r for r in records if r.get("event") == "test_driver" and
             r.get("detail", "").startswith(request_id + " ")]
    start = next((r["t"] for r in marks if r["detail"].split()[1] == "running"), None)
    completed = next((r["t"] for r in marks if r["detail"].split()[1] == "completed"), None)
    errors = []
    if start is None or completed is None:
        return {"passed": False, "errors": ["driver did not complete"], "native_actions_verified": False}
    rows = [(r["t"]-start, r["input"]) for r in records if r.get("type") == "sample" and
            start <= r["t"] <= completed and r.get("input") and r["input"].get("armed") and
            r["input"].get("age_ms", 101) <= 100 and r.get("character", {}).get("age_ms", 101) <= 100]
    phases = {name: [i for t, i in rows if low <= t <= high] for name, low, high in
              (("guard",2.1,2.9),("pistol",4.6,5.4),("light",7.0,7.3),
               ("power",10.0,10.3),("stationary_rev",12.6,14.4))}
    for name, samples in phases.items():
        if len(samples) < 3:
            errors.append(name + ": insufficient fresh observations")
    for name in ("guard", "pistol", "stationary_rev"):
        if any(i["pad"]["buttons"] & (1 | 512) for i in phases[name]):
            errors.append(name + ": unexpected melee button")
    if not any(i["pad"]["buttons"] == 256 for i in phases["guard"]):
        errors.append("left grip did not request parry alone")
    if not any(i["pad"]["rt"] >= 200 and i["pad"]["lt"] == 0 for i in phases["pistol"]):
        errors.append("left pistol trigger did not independently reach RT")
    if not any(i["pad"]["buttons"] == 512 for i in phases["light"]):
        errors.append("released fast sword swing did not request light alone")
    if not any(i["pad"]["buttons"] == 1 for i in phases["power"]):
        errors.append("triggered fast sword swing did not request power alone")
    if any(i["pad"]["rt"] or i["pad"]["lt"] for i in phases["stationary_rev"]):
        errors.append("stationary rev also emitted a firearm trigger")
    if not any(i.get("controls", {}).get("chainsword_rev", 0) >= .99 for i in phases["stationary_rev"]):
        errors.append("stationary held trigger did not request rev")
    return {"passed": not errors, "errors": errors, "samples": {k:len(v) for k,v in phases.items()},
            "native_actions_verified": False,
            "remaining_gate": "Observe owned native melee animation/action state: guard/fire must not attack; light/power must execute and no ping may activate."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("request_id")
    args = parser.parse_args()
    records = []
    for line in args.trace.read_text().splitlines():
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    result = check(records, args.request_id)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
