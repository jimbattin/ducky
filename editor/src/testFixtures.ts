// The repo's tests/fixtures, for the editor's tests.
export const FIXTURES = `${import.meta.dir}/../../tests/fixtures`;

export const readFixture = async (rel: string): Promise<Uint8Array> =>
  new Uint8Array(await Bun.file(`${FIXTURES}/${rel}`).arrayBuffer());

export const readFixtureText = (rel: string): Promise<string> => Bun.file(`${FIXTURES}/${rel}`).text();
