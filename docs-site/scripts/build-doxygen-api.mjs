/**
 * Build the custom API model from Doxygen XML.
 *
 * Doxygen emits presentation compounds (groups, namespaces, files, and
 * directories) alongside public symbols. The site deliberately filters those
 * compounds and resolves member references into a smaller API model.
 */
import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const xmlRoot = resolve(process.cwd(), process.env.DOXYGEN_XML_DIR || '../build/docs/docs/xml');
const outputDir = join(process.cwd(), 'src', 'data');
const outputFile = join(outputDir, 'doxygen-api.json');
const hiddenMemberKinds = new Set(['typedef', 'friend']);
const publicCompoundKinds = new Set(['class', 'struct', 'union', 'enum', 'concept']);

function decode(value = '') {
  return value
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, '$1')
    .replace(/&lt;/g, '<').replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&amp;/g, '&')
}

function rawText(node) {
  if (!node) return '';
  if (node.parts) {
    return node.parts.map((part) => typeof part === 'string' ? part : rawText(part)).join('');
  }
  return `${node.text || ''}${node.children.map(rawText).join('')}`;
}

function text(node) {
  return decode(rawText(node)).replace(/\s+/g, ' ').trim();
}

function proseText(node) {
  if (!node) return '';
  if (node.name === 'parameterlist' || node.name === 'xrefsect') return '';
  if (node.name === 'simplesect' && ['return', 'see', 'retval'].includes(node.attrs.kind)) return '';
  return decode(rawProseText(node))
    .replace(/\s+/g, ' ').trim();
}

function rawProseText(node) {
  if (!node) return '';
  if (node.name === 'parameterlist' || node.name === 'xrefsect') return '';
  if (node.name === 'simplesect' && ['return', 'see', 'retval'].includes(node.attrs.kind)) return '';
  if (node.name === 'simplesect' && node.attrs.kind === 'par') {
    return node.children.map(rawProseText).filter(Boolean).join(' ');
  }
  if (node.parts) {
    return node.parts
      .filter((part) => typeof part === 'string' ||
        (part.name !== 'parameterlist' && part.name !== 'xrefsect' &&
          !(part.name === 'simplesect' && ['return', 'see', 'retval'].includes(part.attrs.kind))))
      .map((part) => typeof part === 'string' ? part : rawProseText(part))
      .join('');
  }
  return `${node.text || ''}${node.children.map(rawProseText).join('')}`;
}

function paragraphs(node) {
  if (!node) return [];
  const paras = children(node, 'para').map(proseText).filter(Boolean);
  return paras.length > 0 ? paras : [proseText(node)].filter(Boolean);
}

function parseXml(xml) {
  const root = { name: '#root', attrs: {}, children: [], parts: [], text: '' };
  const stack = [root];
  const tokenPattern = /<!--[\s\S]*?-->|<!\[CDATA\[[\s\S]*?\]\]>|<\?[\s\S]*?\?>|<\/?[^>]+>|[^<]+/g;
  for (const token of xml.match(tokenPattern) || []) {
    if (token.startsWith('<!--') || token.startsWith('<?') || token.startsWith('<!DOCTYPE')) continue;
    if (token.startsWith('<![CDATA[')) {
      stack.at(-1).text += token.slice(9, -3);
      stack.at(-1).parts.push(token.slice(9, -3));
      continue;
    }
    if (!token.startsWith('<')) {
      stack.at(-1).text += token;
      stack.at(-1).parts.push(token);
      continue;
    }
    if (token.startsWith('</')) {
      stack.pop();
      continue;
    }
    const selfClosing = token.endsWith('/>');
    const body = token.slice(1, selfClosing ? -2 : -1).trim();
    const [, name, attributes = ''] = body.match(/^([^\s]+)([\s\S]*)$/) || [];
    if (!name) continue;
    const attrs = Object.fromEntries([...attributes.matchAll(/([\w:-]+)="([^"]*)"/g)]
      .map(([, key, value]) => [key, decode(value)]));
    const node = { name, attrs, children: [], parts: [], text: '' };
    stack.at(-1).children.push(node);
    stack.at(-1).parts.push(node);
    if (!selfClosing) stack.push(node);
  }
  return root;
}

function children(node, name) {
  return node?.children.filter((child) => child.name === name) || [];
}

function descendants(node, name) {
  return (node?.children || []).flatMap((child) => [
    ...(child.name === name ? [child] : []),
    ...descendants(child, name),
  ]);
}

function first(node, name) {
  return children(node, name)[0];
}

function description(node, name) {
  return text(first(node, name));
}

function slug(value) {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'symbol';
}

function renderableDescription(node, name) {
  const value = description(node, name);
  return value || null;
}

function renderableParagraphs(node, name) {
  const value = paragraphs(first(node, name));
  return value.length > 0 ? value : null;
}

function structuredSections(member) {
  const sections = descendants(member, 'simplesect')
    .filter((section) => ['par', 'see'].includes(section.attrs.kind))
    .map((section) => ({
      kind: section.attrs.kind,
      title: text(first(section, 'title')) || null,
      name: null,
      text: paragraphs(section).join(' '),
    }));
  for (const list of descendants(member, 'parameterlist')) {
    if (list.attrs.kind !== 'retval') continue;
    for (const item of children(list, 'parameteritem')) {
      sections.push({
        kind: 'retval',
        title: null,
        name: descendants(item, 'parametername').map(text).join(', '),
        text: text(first(item, 'parameterdescription')),
      });
    }
  }
  return sections.filter((section) => section.text || section.name);
}

function parameterDocs(member) {
  const docs = new Map();
  for (const list of descendants(member, 'parameterlist')) {
    if (list.attrs.kind !== 'param') continue;
    for (const item of children(list, 'parameteritem')) {
      const names = descendants(item, 'parametername').map(text);
      const value = proseText(first(item, 'parameterdescription'));
      for (const name of names) docs.set(name, value || null);
    }
  }
  return docs;
}

function parseParameter(parameter, docs) {
  const name = text(first(parameter, 'declname')) || text(first(parameter, 'defname'));
  return {
    name,
    type: text(first(parameter, 'type')),
    defaultValue: text(first(parameter, 'defval')) || null,
    description: docs.get(name) || null,
  };
}

function parseMember(member) {
  const docs = parameterDocs(member);
  return {
    id: member.attrs.id || null,
    name: text(first(member, 'name')),
    qualifiedName: text(first(member, 'qualifiedname')),
    kind: member.attrs.kind || 'member',
    protection: member.attrs.prot || null,
    static: member.attrs.static === 'yes',
    const: member.attrs.const === 'yes',
    noexcept: member.attrs.noexcept === 'yes',
    definition: text(first(member, 'definition')) || null,
    type: text(first(member, 'type')) || null,
    args: text(first(member, 'argsstring')) || null,
    brief: renderableDescription(member, 'briefdescription'),
    briefParagraphs: renderableParagraphs(member, 'briefdescription'),
    description: renderableParagraphs(member, 'detaileddescription')?.join('\n\n') || null,
    descriptionParagraphs: renderableParagraphs(member, 'detaileddescription'),
    sections: structuredSections(member),
    parameters: children(member, 'param').map((parameter) => parseParameter(parameter, docs)),
    returnDescription: descendants(member, 'simplesect')
      .find((section) => section.attrs.kind === 'return')
      ? paragraphs(descendants(member, 'simplesect')
        .find((section) => section.attrs.kind === 'return')).join(' ')
      : null,
    enumValues: children(member, 'enumvalue').map((value) => ({
      name: text(first(value, 'name')),
      brief: renderableDescription(value, 'briefdescription'),
    })),
    references: children(member, 'references').map((reference) => reference.attrs.refid).filter(Boolean),
    examples: descendants(member, 'programlisting').map(text).filter(Boolean),
  };
}

function parseCompound(xml, filename) {
  const root = parseXml(xml);
  const compound = descendants(root, 'compounddef')[0];
  if (!compound) return null;
  const name = text(first(compound, 'compoundname'));
  if (!name) return null;
  const location = first(compound, 'location');
  return {
    sourceId: compound.attrs.id,
    name,
    kind: compound.attrs.kind || 'compound',
    brief: renderableDescription(compound, 'briefdescription'),
    briefParagraphs: renderableParagraphs(compound, 'briefdescription'),
    description: renderableParagraphs(compound, 'detaileddescription')?.join('\n\n') || null,
    descriptionParagraphs: renderableParagraphs(compound, 'detaileddescription'),
    sections: structuredSections(compound),
    location: location?.attrs.file ? {
      file: location.attrs.file,
      line: Number(location.attrs.line) || null,
      column: Number(location.attrs.column) || null,
    } : null,
    includes: children(compound, 'includes').map((include) => text(include)).filter(Boolean),
    bases: children(compound, 'basecompoundref').map((base) => ({
      name: text(base),
      refid: base.attrs.refid || null,
    })),
    members: children(compound, 'sectiondef').flatMap((section) =>
      children(section, 'memberdef').map(parseMember)),
    memberRefs: children(compound, 'sectiondef').flatMap((section) =>
      children(section, 'member').map((member) => member.attrs.refid).filter(Boolean)),
    source: filename,
  };
}

const aliases = [
  ['utils', 'Utils', 'Errors, logging, JSON, queues, threads, and versioning'],
  ['types', 'Types', 'Shared value types and wire structures'],
  ['contracts', 'Contracts', 'Backend-neutral persistence interfaces'],
  ['media', 'Media', 'Reading, decoding, metadata, and library discovery'],
  ['player', 'Player', 'Audio output and playback primitives'],
  ['db', 'Database', 'SQLite storage and full-text search'],
  ['engine', 'Engine', 'Queue, shuffle, repeat, history, and playback state'],
  ['ipc', 'IPC', 'Daemon protocol, framing, and configuration'],
  ['client', 'Client', 'Daemon client and output formatting'],
  ['service', 'Service', 'Daemon service and in-process hosting'],
  ['config', 'Config', 'Configuration management'],
];

function isPublicMember(member) {
  return member.protection !== 'private' &&
    member.protection !== 'protected' &&
    !hiddenMemberKinds.has(member.kind) &&
    Boolean(member.name);
}

function build() {
  const parsed = existsSync(xmlRoot)
    ? readdirSync(xmlRoot)
      .filter((file) => file.endsWith('.xml') && file !== 'index.xml')
      .map((file) => parseCompound(readFileSync(join(xmlRoot, file), 'utf8'), file))
      .filter(Boolean)
    : [];
  const memberById = new Map();
  for (const compound of parsed) {
    for (const member of compound.members) {
      if (member.id) memberById.set(member.id, member);
    }
  }

  const publicCompounds = parsed
    .filter((compound) => publicCompoundKinds.has(compound.kind))
    .map((compound) => ({
      ...compound,
      id: `symbol-${slug(compound.name)}-${slug(compound.sourceId)}`,
      members: [...compound.members, ...compound.memberRefs.map((id) => memberById.get(id)).filter(Boolean)]
        .filter(isPublicMember)
        .filter((member, index, list) => list.findIndex((item) => item.id === member.id) === index),
    }));
  const compoundByName = new Map(publicCompounds.map((compound) => [compound.name, compound]));

  const freeFunctions = [];
  for (const member of memberById.values()) {
    if (member.kind !== 'function' || !isPublicMember(member)) continue;
    const owner = member.qualifiedName?.split('::').slice(0, -1).join('::');
    const owningCompound = compoundByName.get(owner);
    if (owningCompound) {
      if (!owningCompound.members.some((item) => item.id === member.id)) owningCompound.members.push(member);
    } else if (member.qualifiedName?.startsWith('caudio::')) {
      freeFunctions.push({
        sourceId: member.id,
        id: `symbol-${slug(member.qualifiedName)}-${slug(member.id)}`,
        name: member.qualifiedName,
        kind: 'function',
        brief: member.brief,
        description: member.description,
        location: null,
        includes: [],
        bases: [],
        members: [member],
        source: 'doxygen-member',
      });
    }
  }

  const allSymbols = [...publicCompounds, ...freeFunctions]
    .sort((a, b) => a.name.localeCompare(b.name));
  mkdirSync(outputDir, { recursive: true });
  writeFileSync(outputFile, JSON.stringify({
    schemaVersion: 3,
    generated: existsSync(xmlRoot),
    source: xmlRoot,
    aliases: aliases.map(([id, name, description]) => ({ id, name, description })),
    compounds: allSymbols,
  }, null, 2));
  console.log(`Doxygen API model: ${allSymbols.length} public symbols${existsSync(xmlRoot) ? '' : ' (XML directory not found)'}`);
}

build();
