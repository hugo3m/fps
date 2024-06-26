# 015

In this version we hook everything back up so that player state packets are returned to the client for each input packet received.

I expect this will slightly reduce throughput, but hopefully the ring buffer comes out ahead of the ~3k players that could be processed with the perf buffer.

Also, since I'm not seeing any throughput benefits from directly polling the ring buffer consume on the worker threads, I'm going to go back to using epoll so we free up some CPU.

Since it seems that the best result is to process packets on the CPU that receives them, we'll need some spare cycles on each CPU in order to do actual work for player simulation.

# Results

Success. We are able to scale up to 4k players per-player server:

```
Apr 28 16:49:44 client-9plv client[11221]: inputs sent delta 99396, inputs processed delta 398186, player state delta 99391
Apr 28 16:49:45 client-9plv client[11221]: inputs sent delta 99342, inputs processed delta 398545, player state delta 99345
Apr 28 16:49:46 client-9plv client[11221]: inputs sent delta 99303, inputs processed delta 398427, player state delta 99304
Apr 28 16:49:47 client-9plv client[11221]: inputs sent delta 99298, inputs processed delta 397852, player state delta 99300
Apr 28 16:49:48 client-9plv client[11221]: inputs sent delta 100000, inputs processed delta 397646, player state delta 99998
```

Lessons learned:

1. The ring buffer is definitely faster than the perf buffer (saves a copy, and memory bandwidth is definitely a limiting factor here)
2. It's best to process inputs and player state on the same CPU that received the traffic (avoid contention and NUMA bottlenecks)
3. There's lots of CPU remaining to run player simulation, even on 16 CPUs.

<img width="1002" alt="image" src="https://github.com/mas-bandwidth/fps/assets/696656/c4e7dab6-cb6c-42da-bef9-d2d25a9110c1">

In short, the application is currently IO bound and we have plenty of CPU left.

This is great news, because we can reduce our player server down from 64 cores to 32 cores.

Assume that we can get double the results on the bare metal that we can get on the VM, so 4 -> 8k player per-player server.

A 32 core machine only costs $1,870 USD per-month.

We need 125 machines if we can fit 8k players on each player server.

Now the total cost is $233,750 USD per-month, or just 23.4c per-player per-month.
