/**
 * Build search index for caudio docs site.
 * Reads markdown files directly from src/content/docs/ and writes public/search-index.json
 * Run: node scripts/build-search-index.mjs
 */

import { readdirSync, readFileSync, existsSync, mkdirSync, writeFileSync } from 'fs';
import { join, extname, relative, dirname } from 'path';

const docsRoot = join(process.cwd(), 'src', 'content', 'docs');
const publicDir = join(process.cwd(), 'public');

function parseFrontmatter(content) {
  const match = content.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n/);
  if (!match) return {};
  const fm = {};
  for (const line of match[1].split('\n')) {
    const colonIdx = line.indexOf(':');
    if (colonIdx > 0) {
      const key = line.slice(0, colonIdx).trim();
      let val = line.slice(colonIdx + 1).trim();
      // Remove surrounding quotes
      if ((val.startsWith('"') && val.endsWith('"')) || (val.startsWith("'") && val.endsWith("'"))) {
        val = val.slice(1, -1);
      }
      fm[key] = val;
    }
  }
  return fm;
}

function stripMarkdown(md) {
  return md
    .replace(/^---\r?\n[\s\S]*?\r?\n---\r?\n/, '')
    .replace(/```[\w-]*\r?\n([\s\S]*?)```/g, '$1')
    .replace(/!\[[^\]]*\]\([^)]*\)/g, ' ')
    .replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
    .replace(/<[^>]+>/g, ' ')
    .replace(/^#{1,6}\s+/gm, '')
    .replace(/[*_~]/g, '')
    .replace(/`([^`]+)`/g, '$1')
    .replace(/\s+/g, ' ')
    .trim();
}

function walkDir(dir, fileList = []) {
  const entries = readdirSync(dir, { withFileTypes: true });
  for (const entry of entries) {
    const fullPath = join(dir, entry.name);
    if (entry.isDirectory()) {
      walkDir(fullPath, fileList);
    } else if (entry.isFile() && extname(entry.name) === '.md') {
      fileList.push(fullPath);
    }
  }
  return fileList;
}

function slugToUrl(slug) {
  if (slug === 'index') return '/';
  if (slug.endsWith('/index')) return `/${slug.slice(0, -6)}/`;
  return `/${slug}/`;
}

function normalize(value) {
  return value
    .toLowerCase()
    .normalize('NFKD')
    .replace(/[^\p{Letter}\p{Number}]+/gu, ' ')
    .trim();
}

async function build() {
  if (!existsSync(docsRoot)) {
    console.error(`❌ Docs root not found: ${docsRoot}`);
    process.exit(1);
  }

  const files = walkDir(docsRoot);
  const index = [];

  for (const filePath of files) {
    const content = readFileSync(filePath, 'utf-8');
    const fm = parseFrontmatter(content);

    // Compute slug from relative path
    const relPath = relative(docsRoot, filePath).replace(/\\/g, '/');
    const slug = relPath.replace(/\.md$/, '');
    const url = slugToUrl(slug);

    const body = stripMarkdown(content);

    const title = fm.title || slug;
    const description = fm.description || '';
    index.push({
      title,
      description,
      content: body,
      url,
      section: slug.split('/')[0],
      searchText: normalize(`${title} ${description} ${body}`),
    });
  }

  if (!existsSync(publicDir)) {
    mkdirSync(publicDir, { recursive: true });
  }

  writeFileSync(
    join(publicDir, 'search-index.json'),
    JSON.stringify(index, null, 2),
    'utf-8'
  );

  console.log(`✅ Search index built: ${index.length} pages`);
}

build().catch((err) => {
  console.error('❌ Failed to build search index:', err);
  process.exit(1);
});
