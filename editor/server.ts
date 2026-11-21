import editor from "./src/editor.html";

const port = Number(process.env.PORT ?? 3000);
Bun.serve({ port, routes: { "/": editor }, development: { hmr: true } });
console.log(`Ducky editor: http://localhost:${port}/`);
