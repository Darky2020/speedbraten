#!/usr/bin/env python3
"""Legacy campaign records from speedrun.com/sauerbraten.

    tools/legacycampaigns.py > campaigns.csv

Each of the game's categories there is one campaign; its record is the leaderboard's first
place, whatever difficulty it was run on. The output is the admin site's legacy CSV:
map,kind,mode,player,time,source,date,link, the map being the campaign's first (the boards
file a campaign under it).
"""
import json
import sys
import time
import urllib.request

API = 'https://www.speedrun.com/api/v1'
# speedrun.com's category name -> the campaign's first map (fpsgame/runtimer.cpp campaigns)
CAMPAIGNS = {'An Army Of One': 'mpsp6a', 'Private Stan Sauer': 'mpsp9a', 'Lost': 'lost',
             'Meltdown': 'skrsp1', 'Missile Pass': 'crnsp1', 'Level 9': 'level9'}
# a player's speedrun.com name -> the one shown on the boards
NAMES = {'ImRaze': 'Raze'}


def get(path):
    req = urllib.request.Request(API + path, headers={'User-Agent': 'speedbraten legacy records'})
    with urllib.request.urlopen(req) as r:
        return json.load(r)['data']


def main():
    game = get('/games?abbreviation=sauerbraten')[0]['id']
    for cat in get(f'/games/{game}/categories'):
        mapname = CAMPAIGNS.get(cat['name'])
        if not mapname or cat['type'] != 'per-game':
            print(f'skipped category {cat["name"]!r}', file=sys.stderr)
            continue
        time.sleep(0.7)  # speedrun.com allows about 100 requests a minute
        board = get(f'/leaderboards/{game}/category/{cat["id"]}?top=1&embed=players')
        runs = [r for r in board['runs'] if r['place'] == 1]
        if not runs:
            print(f'{cat["name"]}: no runs', file=sys.stderr)
            continue
        run = runs[0]['run']
        players = board['players']['data']
        name = ' & '.join((p.get('names') or {}).get('international') or p.get('name', '?') for p in players)
        name = NAMES.get(name, name)
        ms = round(run['times']['primary_t'] * 1000)
        t = f'{ms // 60000}:{ms // 1000 % 60:02d}.{ms % 1000:03d}'
        print(f'{mapname},campaign,-3,{name},{t},speedrun.com,{run.get("date") or ""},{run["weblink"]}')


if __name__ == '__main__':
    main()
