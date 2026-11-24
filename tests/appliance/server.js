import { readFileSync, writeFileSync } from 'node:fs';

Ant.serve({
  hostname: '0.0.0.0',
  port: 8080,
  fetch(req) {
    switch (new URL(req.url).pathname) {
      case '/system-memory':
        return new Response(readFileSync('/proc/meminfo', 'utf8'));
      case '/memory':
        return new Response(readFileSync('/proc/self/status', 'utf8'));
      case '/save':
        writeFileSync('/data/value', 'written by ' + process.env.REVISION);
        return new Response('saved');
      case '/load':
        return new Response(readFileSync('/data/value', 'utf8'));
      case '/quit':
        setTimeout(() => process.exit(0), 100);
        return new Response('bye');
      default:
        return new Response(process.env.REVISION);
    }
  }
});
