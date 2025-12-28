import os
import re
import json
import time
import hashlib
import requests
from collections import deque

API = "https://en.wikipedia.org/w/api.php"

HEADERS = {
    "User-Agent": "MAI-SearchLab/1.0 (contact: student@example.com)"
}

def norm_title(t: str) -> str:
    return t.replace("_", " ").strip()

def words_count(text: str) -> int:
    # очень простой подсчёт слов
    return len(re.findall(r"[A-Za-zА-Яа-яЁё0-9]+", text))

def api_get(params: dict) -> dict:
    r = requests.get(API, params=params, headers=HEADERS, timeout=60)
    r.raise_for_status()
    return r.json()

def list_category_members(cat_title: str, cmtype: str):
    cont = None
    while True:
        params = {
            "action": "query",
            "list": "categorymembers",
            "cmtitle": cat_title,
            "cmtype": cmtype,
            "cmlimit": "500",
            "format": "json",
        }
        if cont:
            params["cmcontinue"] = cont

        data = api_get(params)
        for it in data.get("query", {}).get("categorymembers", []):
            yield it  # содержит pageid, ns, title

        cont = data.get("continue", {}).get("cmcontinue")
        if not cont:
            break

def get_plaintext_by_title(title: str) -> tuple[int, str, str]:

    params = {
        "action": "query",
        "prop": "extracts",
        "explaintext": "1",
        "format": "json",
        "titles": title,
        "redirects": "1",
    }
    data = api_get(params)
    pages = data.get("query", {}).get("pages", {})
    # pages: {pageid: {...}}
    for pid_str, page in pages.items():
        pid = int(pid_str)
        if "missing" in page:
            return pid, title, ""
        text = page.get("extract", "") or ""
        real_title = page.get("title", title)
        return pid, real_title, text
    return -1, title, ""

def stable_doc_id(pageid: int, title: str) -> str:

    h = hashlib.sha1(f"{pageid}:{title}".encode("utf-8")).hexdigest()
    return h[:12]

from collections import deque
import time

def dry_run_count(root_category: str, limit_pages: int | None = None, limit_cats: int | None = None):

    seen_cats = set()
    seen_pages = set()

    q = deque([root_category])

    processed_cats = 0

    while q:
        cat = q.popleft()
        if cat in seen_cats:
            continue
        seen_cats.add(cat)
        processed_cats += 1


        for it in list_category_members(cat, "subcat"):

            sub = it["title"]
            if sub not in seen_cats:
                q.append(sub)

        for it in list_category_members(cat, "page"):
            if it.get("ns", 0) != 0:
                continue
            seen_pages.add(it["pageid"])

            if limit_pages is not None and len(seen_pages) >= limit_pages:
                return {
                    "categories_processed": processed_cats,
                    "categories_seen": len(seen_cats),
                    "unique_pages": len(seen_pages),
                    "stopped_reason": f"reached limit_pages={limit_pages}",
                }

        if limit_cats is not None and processed_cats >= limit_cats:
            return {
                "categories_processed": processed_cats,
                "categories_seen": len(seen_cats),
                "unique_pages": len(seen_pages),
                "stopped_reason": f"reached limit_cats={limit_cats}",
            }

        time.sleep(0.05)

    return {
        "categories_processed": processed_cats,
        "categories_seen": len(seen_cats),
        "unique_pages": len(seen_pages),
        "stopped_reason": "queue exhausted (finished traversal)",
    }


def build_corpus(root_category: str, out_dir: str, need_docs: int, min_words: int):
    os.makedirs(out_dir, exist_ok=True)
    corpus_dir = os.path.join(out_dir, "corpus")
    os.makedirs(corpus_dir, exist_ok=True)

    manifest_path = os.path.join(out_dir, "manifest.jsonl")

    seen_cats = set()
    seen_pages = set()

    q = deque([root_category])
    kept = 0

    with open(manifest_path, "w", encoding="utf-8") as mf:
        while q and kept < need_docs:
            cat = q.popleft()
            if cat in seen_cats:
                continue
            seen_cats.add(cat)

            for it in list_category_members(cat, "subcat"):
                sub = it["title"]
                if sub not in seen_cats:
                    q.append(sub)

            for it in list_category_members(cat, "page"):
                if kept >= need_docs:
                    break
                pageid = it["pageid"]
                title = norm_title(it["title"])
                if pageid in seen_pages:
                    continue
                seen_pages.add(pageid)

                pid, real_title, text = get_plaintext_by_title(title)

                wc = words_count(text)
                if wc < min_words:
                    continue

                doc_id = stable_doc_id(pid, real_title)
                doc_path = os.path.join(corpus_dir, f"{doc_id}.txt")

                with open(doc_path, "w", encoding="utf-8") as f:
                    f.write(text)

                rec = {
                    "doc_id": doc_id,
                    "pageid": pid,
                    "title": real_title,
                    "category_seed": root_category,
                    "word_count": wc,
                    "url": f"https://en.wikipedia.org/wiki/{real_title.replace(' ', '_')}",
                    "source": "en.wikipedia.org",
                }
                mf.write(json.dumps(rec, ensure_ascii=False) + "\n")

                kept += 1

                time.sleep(0.1)

    print(f"Done. Saved {kept} docs to {out_dir}")

if __name__ == "__main__":
    build_corpus(
        root_category="https://en.wikipedia.org/wiki/Category:Applied_mathematics",
        out_dir="out_wiki_corpus",
        need_docs=15000,
        min_words=1100,
    )