zero := 0 : stored('zero');
d := dataset([
        {0},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9},
        {1},{1},{2},{3},{4},{8},{9}
        ], { unsigned r; }, DISTRIBUTED);
p2 := TABLE(NOFOLD(d), { COUNT(group) }, LOCAL);
output(count(p2) = CLUSTERSIZE);