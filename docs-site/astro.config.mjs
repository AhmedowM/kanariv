import { defineConfig } from 'astro/config';

// GitHub Pages project site: https://ahmedowm.github.io/kanariv/
// Keep this in sync with the repository name.
const base = '/kanariv/';

/**
 * Astro does not rewrite root-relative links written in Markdown, so prefix
 * them with the deployment base at build time. External URLs, protocol-relative
 * URLs, and links that already carry the base are left alone.
 */
function rehypeBaseLinks() {
  const prefix = base.replace(/\/$/, '');

  const walk = (node) => {
    if (
      node.type === 'element' &&
      (node.tagName === 'a' || node.tagName === 'img')
    ) {
      const attribute = node.tagName === 'a' ? 'href' : 'src';
      const value = node.properties?.[attribute];
      if (
        typeof value === 'string' &&
        value.startsWith('/') &&
        !value.startsWith('//') &&
        value !== prefix &&
        !value.startsWith(`${prefix}/`)
      ) {
        node.properties[attribute] = `${prefix}${value}`;
      }
    }
    if (Array.isArray(node.children)) {
      node.children.forEach(walk);
    }
  };

  return (tree) => walk(tree);
}

export default defineConfig({
  site: 'https://ahmedowm.github.io',
  base,
  output: 'static',
  markdown: {
    rehypePlugins: [rehypeBaseLinks],
  },
});
