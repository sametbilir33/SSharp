int First(int[] values)
{
    return values[0];
}

int[] numbers = { 5, 2, 8, 1 };
Console.WriteLine(numbers.Length);
Console.WriteLine(First(numbers));
Console.WriteLine(numbers[0]);
Console.WriteLine(numbers[1]);
numbers[1] = 9;
Console.WriteLine(numbers[1]);

int sum = 0;
for (int i = 0; i < numbers.Length; i = i + 1)
{
    sum = sum + numbers[i];
}
Console.WriteLine(sum);

bool[] flags = { true, false };
Console.WriteLine(flags.Length);
Console.WriteLine(flags[0]);
flags[1] = true;
Console.WriteLine(flags[1]);

string[] names = { "Samet", "SSharp" };
Console.WriteLine(names.Length);
Console.WriteLine(names[0]);
names[1] = "Arrays";
Console.WriteLine(names[1]);

int[] zeroed = new int[3];
Console.WriteLine(zeroed.Length);
Console.WriteLine(zeroed[0]);
zeroed[2] = 7;
Console.WriteLine(zeroed[2]);

bool[] empty = new bool[0];
Console.WriteLine(empty.Length);

int[] operations = { 4, 2, 4, 1 };
Console.WriteLine(Array.IndexOf(operations, 4));
Console.WriteLine(Array.LastIndexOf(operations, 4));
Array.Sort(operations);
Console.WriteLine(operations[0]);
Console.WriteLine(operations[1]);
Array.Reverse(operations);
Console.WriteLine(operations[0]);

int[] copied = new int[4];
Array.Copy(operations, 1, copied, 0, 3);
Console.WriteLine(copied[0]);
Console.WriteLine(copied[2]);
Array.Copy(copied, 0, copied, 1, 3);
Console.WriteLine(copied[1]);
Console.WriteLine(copied[3]);

Array.Clear(copied, 1, 2);
Console.WriteLine(copied[1]);
Array.Fill(copied, 8);
Console.WriteLine(copied[0]);
Array.Fill(copied, 6, 1, 2);
Console.WriteLine(copied[1]);
Console.WriteLine(copied[3]);

operations = Array.Resize(operations, 6);
Console.WriteLine(operations.Length);
Console.WriteLine(operations[4]);
operations[4] = 9;
operations = Array.Resize(operations, 2);
Console.WriteLine(operations.Length);
Console.WriteLine(operations[1]);

bool[] sortedFlags = { true, false, true };
Array.Sort(sortedFlags);
Console.WriteLine(sortedFlags[0]);
Console.WriteLine(Array.IndexOf(sortedFlags, true));

string[] sortedNames = { "zebra", "apple", "mango" };
Array.Sort(sortedNames);
Console.WriteLine(sortedNames[0]);
Console.WriteLine(Array.IndexOf(sortedNames, "mango"));

string[] nullValues = { null, "" };
Console.WriteLine(Array.IndexOf(nullValues, null));
Console.WriteLine(Array.IndexOf(nullValues, ""));
